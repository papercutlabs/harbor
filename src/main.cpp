// main.cpp
//
// Copyright (c) 2019-2026 Kristofer Berggren
// All rights reserved.
//
// nchat is distributed under the MIT license, see LICENSE for details.

#include <algorithm>
#include <iostream>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <string>

#include <cassert>
#include <dlfcn.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include "appconfig.h"
#include "clipboard.h"
#include "apputil.h"
#include "debuginfo.h"
#include "fileutil.h"
#include "log.h"
#include "messagecache.h"
#include "profiles.h"
#include "scopeddirlock.h"
#include "status.h"
#include "strutil.h"
#include "sysutil.h"
#include "ui.h"

#ifdef HAS_DUMMY
#include "duchat.h"
#endif

#ifdef HAS_TELEGRAM
#include "tgchat.h"
#endif

#ifdef HAS_WHATSAPP
#include "wmchat.h"
#endif

#ifdef HAS_SIGNAL
#include "sgchat.h"
#endif

#ifdef HARBOR_BUILD
#include "harbor/demoprotocol.h"
#include "harbor/update.h"
#include "harbor/restart.h"
#include "harbor/session.h"
#include "harbor/trace.h"
#include "harbor/previewguard.h"
#include "harbor/launchroute.h"
#include "harbor/uicontrol.h"
static bool harborDemo = false;
static int RunHarborWindow(const std::vector<std::string>& p_Args, const HarborRoute::Target& p_Target);
#endif

static std::string GetFeatures();
static void RemoveProfile();
static std::shared_ptr<Protocol> SetupProfile();
static void ShowHelp();
static void ShowVersion();


class ProtocolBaseFactory
{
public:
  ProtocolBaseFactory() { }
  virtual ~ProtocolBaseFactory() { }
  virtual std::string GetName() const = 0;
  virtual std::string GetSetupMessage() const = 0;
  virtual std::shared_ptr<Protocol> Create() const = 0;
};

template<typename T>
class ProtocolFactory : public ProtocolBaseFactory
{
public:
  virtual std::string GetName() const
  {
    return T::GetName();
  }

  virtual std::string GetSetupMessage() const
  {
    return T::GetSetupMessage();
  }

  virtual std::shared_ptr<Protocol> Create() const
  {
    std::shared_ptr<T> protocol;
#if defined(HAS_DYNAMICLOAD)
#ifdef HARBOR_BUILD
    if (T::GetName() == "HarborDemo") return std::make_shared<T>();
#endif
    std::string libPath =
      FileUtil::DirName(FileUtil::GetSelfPath()) + "/../" CMAKE_INSTALL_LIBDIR "/" + T::GetLibName() +
      FileUtil::GetLibSuffix();
    std::string createFunc = T::GetCreateFunc();
    void* handle = dlopen(libPath.c_str(), RTLD_LAZY);
    if (handle == nullptr)
    {
      LOG_ERROR("failed dlopen %s", libPath.c_str());
      const char* dlerr = dlerror();
      if (dlerr != nullptr)
      {
        LOG_ERROR("dlerror %s", dlerr);
#ifdef HARBOR_BUILD
        HarborTrace::Event("profile.library.failed", {{"diagnostic", dlerr}});
#endif
      }

      std::cout << "Failed to load " << libPath << ", skipping profile.\n";
      return protocol;
    }

    // intentionally leak 'handle' as dlclose may cause exit crash as go routines are not explicitly managed

    T* (* CreateFunc)() = (T * (*)())dlsym(handle, createFunc.c_str());
    if (CreateFunc == nullptr)
    {
      LOG_ERROR("failed dlsym %s", createFunc.c_str());
      return protocol;
    }

    protocol.reset(CreateFunc());
#else
    protocol = std::make_shared<T>();
#endif
    return protocol;
  }
};

static std::vector<std::shared_ptr<ProtocolBaseFactory>> GetProtocolFactorys()
{
#ifdef HARBOR_BUILD
  if (harborDemo) return { std::make_shared<ProtocolFactory<HarborDemo>>() };
#endif
  std::vector<std::shared_ptr<ProtocolBaseFactory>> protocolFactorys =
  {
#ifdef HAS_DUMMY
    std::shared_ptr<ProtocolBaseFactory>(new ProtocolFactory<DuChat>()),
#endif
#ifdef HAS_TELEGRAM
    std::shared_ptr<ProtocolBaseFactory>(new ProtocolFactory<TgChat>()),
#endif
#ifdef HAS_WHATSAPP
    std::shared_ptr<ProtocolBaseFactory>(new ProtocolFactory<WmChat>()),
#endif
#ifdef HAS_SIGNAL
    std::shared_ptr<ProtocolBaseFactory>(new ProtocolFactory<SgChat>()),
#endif
  };

  return protocolFactorys;
}


static int ApplicationMain(int argc, char* argv[])
{
  // Defaults
  umask(S_IRWXG | S_IRWXO);
#ifdef HARBOR_BUILD
  const char* home = getenv("HOME");
  const char* xdg = getenv("XDG_CONFIG_HOME");
  const std::string configHome = (xdg && *xdg) ? xdg : std::string(home ? home : "") + "/.config";
  FileUtil::SetApplicationDir(configHome + "/harbor");
#else
  FileUtil::SetApplicationDir(FileUtil::GetDefaultApplicationDir());
#endif
  Log::SetVerboseLevel(Log::INFO_LEVEL);

  // Argument handling
  std::string exportDir;
  bool isKeyDump = false;
  bool isRemove = false;
  bool isSetup = false;
  bool isExtraDeveloperMode = false;
  std::vector<std::string> args(argv + 1, argv + argc);
#ifdef HARBOR_BUILD
  // Mode flags cannot become another option's value. Recognize and remove this one before
  // parsing any directory/export/route value, matching main's read-only trace decision.
  const bool checkSession = std::find(args.begin(), args.end(), "--check-session") != args.end();
  args.erase(std::remove(args.begin(), args.end(), "--check-session"), args.end());
  // Internal backend entry: harbor --session-backend <fd> [args]. Never documented as a command.
  int sessionFd = -1;
  int sessionShutdownLock = -1;
  bool sessionBackend = false;
  if (!args.empty() && args.front() == HarborSession::BackendArg)
  {
    if (args.size() < 2)
      return 1;
    sessionFd = atoi(args[1].c_str());
    sessionBackend = true;
    args.erase(args.begin(), args.begin() + 2);
  }
  std::vector<std::string> windowArgs; // forwarded to a spawned backend and to a restart
  HarborRoute::Target initialTarget;
  std::string targetSurface, targetPid, uiAction, uiToken;
  bool showHelp = false, showVersion = false, isExport = false;
#endif
  for (auto it = args.begin(); it != args.end(); ++it)
  {
#ifdef HARBOR_BUILD
    if (*it == "--demo")
    {
      harborDemo = true;
      FileUtil::SetApplicationDir(configHome + "/harbor-demo");
      continue;
    }
    if (checkSession && (*it == "-d" || *it == "--confdir" || *it == "-x" || *it == "--export") &&
        std::distance(it + 1, args.end()) <= 0)
    { std::cerr << *it << " requires a value.\n"; return 1; }
#endif
    if (((*it == "-d") || (*it == "--confdir")) && (std::distance(it + 1, args.end()) > 0))
    {
      ++it;
      FileUtil::SetApplicationDir(*it);
    }
#ifdef HARBOR_BUILD
    else if (*it == "--inspect-ui") { if (!uiAction.empty()) { std::cerr << "Choose one UI control operation.\n"; return 1; } uiAction = "inspect"; }
    else if (*it == "--inspect-window") { if (!uiAction.empty()) { std::cerr << "Choose one UI control operation.\n"; return 1; } uiAction = "inspect-window"; }
    else if (*it == "--park-ui")
    {
      if (std::distance(it + 1, args.end()) <= 0) { std::cerr << "--park-ui requires the inspected token.\n"; return 1; }
      if (!uiAction.empty()) { std::cerr << "Choose one UI control operation.\n"; return 1; }
      uiAction = "park"; uiToken = *++it;
    }
    else if (*it == "--open-telegram-chat" || *it == "--open-telegram-topic" || *it == "--open-profile" || *it == "--target-surface" || *it == "--target-pid")
    {
      const auto flag = *it;
      if (std::distance(it + 1, args.end()) <= 0)
      { std::cerr << flag << " requires a value.\n"; return 1; }
      auto& value = flag == "--open-telegram-chat" ? initialTarget.chat : flag == "--open-telegram-topic" ? initialTarget.topic : flag == "--open-profile" ? initialTarget.profile : flag == "--target-surface" ? targetSurface : targetPid;
      if (!value.empty()) { std::cerr << flag << " may be supplied only once.\n"; return 1; }
      value = *++it;
      if (value.empty()) { std::cerr << flag << " requires a nonempty value.\n"; return 1; }
    }
    else if ((*it == "-e") || (*it == "--verbose") || (*it == "-ee") || (*it == "--extra-verbose"))
    {
      Log::SetVerboseLevel(((*it == "-e") || (*it == "--verbose")) ? Log::DEBUG_LEVEL : Log::TRACE_LEVEL);
      windowArgs.push_back(*it);
    }
#endif
    else if ((*it == "-e") || (*it == "--verbose"))
    {
      Log::SetVerboseLevel(Log::DEBUG_LEVEL);
    }
    else if ((*it == "-ee") || (*it == "--extra-verbose"))
    {
      Log::SetVerboseLevel(Log::TRACE_LEVEL);
    }
    else if ((*it == "-h") || (*it == "--help"))
    {
#ifdef HARBOR_BUILD
      showHelp = true;
#else
      ShowHelp();
      return 0;
#endif
    }
    else if ((*it == "-k") || (*it == "--keydump"))
    {
      isKeyDump = true;
    }
    else if ((*it == "-m") || (*it == "--devmode"))
    {
      AppUtil::SetDeveloperMode(true);
    }
    else if ((*it == "-mm") || (*it == "--extra-devmode"))
    {
      isExtraDeveloperMode = true;
    }
    else if ((*it == "-r") || (*it == "--remove"))
    {
      isRemove = true;
    }
    else if ((*it == "-s") || (*it == "--setup"))
    {
      isSetup = true;
    }
    else if ((*it == "-v") || (*it == "--version"))
    {
#ifdef HARBOR_BUILD
      showVersion = true;
#else
      ShowVersion();
      return 0;
#endif
    }
    else if (((*it == "-x") || (*it == "--export")) && (std::distance(it + 1, args.end()) > 0))
    {
#ifdef HARBOR_BUILD
      isExport = true;
#endif
      ++it;
      exportDir = *it;
    }
    else
    {
      ShowHelp();
      return 1;
    }
  }

  if (isExtraDeveloperMode
#ifdef HARBOR_BUILD
      && !checkSession
#endif
      )
  {
    std::cout << "dev mode starting in 5 sec\n";
    sleep(5);
    AppUtil::SetDeveloperMode(true);
  }

#ifdef HARBOR_BUILD
  if (checkSession && (sessionBackend || isSetup || isRemove || isKeyDump || isExport ||
                       !targetSurface.empty() || !targetPid.empty() || !uiAction.empty() || showHelp || showVersion))
  { std::cerr << "--check-session applies only to ordinary Harbor startup arguments; setup, removal, export, keydump, backend, UI control, help and version modes cannot be combined with it.\n"; return 1; }
  if (showHelp) { ShowHelp(); return 0; }
  if (showVersion) { ShowVersion(); return 0; }
  try { HarborRoute::Validate(initialTarget); }
  catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
  if (checkSession)
  {
    if (FileUtil::GetApplicationDir().empty())
    { std::cerr << "--check-session requires a nonempty configuration directory.\n"; return 1; }
    const auto error = HarborSession::CheckSession(FileUtil::GetApplicationDir());
    if (!error.empty()) { std::cerr << error << '\n'; return 1; }
    return 0;
  }
  HarborTrace::Event("startup.selected", {{"role", sessionFd >= 0 ? "backend" : "window"},
    {"runtime", harborDemo ? "demo" : "telegram"}, {"config_id", HarborTrace::Token(FileUtil::GetApplicationDir())},
    {"mode", isSetup ? "setup" : isRemove ? "remove" : isKeyDump ? "keydump" : !exportDir.empty() ? "export" : "normal"}});
  if (initialTarget.Present() && (sessionFd >= 0 || isSetup || isRemove || isKeyDump || !exportDir.empty()))
  { std::cerr << "Initial conversation selection applies only to an ordinary Harbor window.\n"; return 1; }
  if (!targetSurface.empty() || !targetPid.empty() || !uiAction.empty())
  {
    if (targetSurface.empty() || targetPid.empty())
    { std::cerr << "--target-surface and --target-pid must identify the same existing Harbor UI.\n"; return 1; }
    try {
      if (!uiAction.empty() && (initialTarget.Present() || sessionBackend || isSetup || isRemove || isKeyDump || isExport || checkSession))
        throw std::invalid_argument("UI inspection and parking require only an exact existing UI target.");
      const auto response = HarborUiControl::Send(FileUtil::GetApplicationDir(), targetSurface, targetPid, initialTarget, uiAction, uiToken);
      if (uiAction == "inspect" || uiAction == "inspect-window") { std::cout << response; return 0; }
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    std::cout << (uiAction == "park" ? "Duplicate view returned to Chats; drafts retained.\n" : "Requested conversation opened in the selected Harbor UI.\n");
    return 0;
  }
  // An ordinary run is a window of the directory's shared session. Setup, removal, export and
  // keydump keep the classic path under the existing locks.
  if ((sessionFd < 0) && !isSetup && !isRemove && !isKeyDump && exportDir.empty())
  {
    // Creating a directory is non-destructive. Its version is initialised only by
    // the backend under the existing native directory lock.
    FileUtil::MkDir(FileUtil::GetApplicationDir());
    std::vector<std::string> backendArgs;
    if (harborDemo) backendArgs.push_back("--demo");
    backendArgs.push_back("-d");
    backendArgs.push_back(FileUtil::GetApplicationDir());
    backendArgs.insert(backendArgs.end(), windowArgs.begin(), windowArgs.end());
    return RunHarborWindow(backendArgs, initialTarget);
  }
#endif

  bool isDirInited = false;
  static const int dirVersion = 1;
  if (!FileUtil::Exists(FileUtil::GetApplicationDir()))
  {
    FileUtil::InitDirVersion(FileUtil::GetApplicationDir(), dirVersion);
    isDirInited = true;
  }


#ifdef HARBOR_BUILD
  HarborTrace::Event("directory.lock.attempt");
#endif
  std::unique_ptr<ScopedDirLock> dirLock = std::make_unique<ScopedDirLock>(FileUtil::GetApplicationDir());
  if (!dirLock->IsLocked())
  {
#ifdef HARBOR_BUILD
    HarborTrace::Os("directory.lock.failed", dirLock->Error(), {{"operation", dirLock->Operation()}});
    HarborSession::FailBackend(sessionFd, "Harbor could not lock its configuration directory (" + std::string(dirLock->Operation()) +
      "): " + std::strerror(dirLock->Error()) + ".");
#endif
#ifdef HARBOR_BUILD
    std::cerr << "Harbor could not lock its configuration directory (" << dirLock->Operation() << "): "
              << std::strerror(dirLock->Error()) << ".\n";
#else
    std::cerr <<
      "error: unable to acquire lock for " << FileUtil::GetApplicationDir() << "\n" <<
      "       only one nchat session per account/confdir is supported.\n";
#endif
    return 1;
  }

#ifdef HARBOR_BUILD
  HarborTrace::Event("directory.lock.acquired");
#endif
  if (!isDirInited)
  {
    int storedVersion = FileUtil::GetDirVersion(FileUtil::GetApplicationDir());
#ifdef HARBOR_BUILD
    if (storedVersion == 0 && !FileUtil::Exists(FileUtil::GetApplicationDir() + "/version"))
    {
      const auto entries = FileUtil::ListDirNames(FileUtil::GetApplicationDir());
      const bool empty = std::all_of(entries.begin(), entries.end(),
        [](const std::string& name) { return name == "session.lock"; });
      if (empty)
      {
        FileUtil::WriteFile(FileUtil::GetApplicationDir() + "/version", StrUtil::StrToHex(std::to_string(dirVersion)));
        storedVersion = FileUtil::GetDirVersion(FileUtil::GetApplicationDir());
      }
    }
#endif
    if (storedVersion != dirVersion)
    {
#ifdef HARBOR_BUILD
      HarborTrace::Event("directory.version.rejected", {{"stored_version", std::to_string(storedVersion)}, {"expected_version", std::to_string(dirVersion)}});
      HarborSession::FailBackend(sessionFd, "Harbor will not replace an existing directory with an unknown format.\n"
                                 "Choose a new configuration directory with -d, or restore its version file.");
      std::cerr << "Harbor will not replace an existing directory with an unknown format.\n"
                   "Choose a new configuration directory with -d, or restore its version file.\n";
      return 1;
#endif
      if (isSetup)
      {
        FileUtil::InitDirVersion(FileUtil::GetApplicationDir(), dirVersion);
      }
      else
      {
        std::cerr << "error: invalid config dir content, exiting. use -s to setup nchat.\n";
        return 1;
      }
    }
  }

  // Remove profile
  if (isRemove)
  {
    RemoveProfile();
    return 0;
  }

  // Init profiles dir
  Profiles::Init();
#ifdef HARBOR_BUILD
  if (harborDemo)
    FileUtil::MkDir(FileUtil::GetApplicationDir() + "/profiles/HarborDemo_local");
#endif

  // Init logging
  const std::string& logPath = FileUtil::GetApplicationDir() + std::string("/log.txt");
  Log::Init(logPath);
#ifdef HARBOR_BUILD
  HarborTrace::Event("native.logging.ready");
#endif
  std::string appNameVersion = AppUtil::GetAppName(true /*p_WithVersion*/, true /*p_WithBranch*/);
  LOG_INFO("%s", appNameVersion.c_str());
  std::string osArch = SysUtil::GetOsArch(false /*p_Verbose*/);
  LOG_INFO("%s", osArch.c_str());
  std::string buildInfo = SysUtil::GetBuildInfo();
  LOG_INFO("%s", buildInfo.c_str());
  std::string compiler = SysUtil::GetCompiler(false /*p_Verbose*/);
  LOG_INFO("%s", compiler.c_str());
  std::string go = SysUtil::GetGo(GO_VERSION);
  LOG_INFO("%s", go.c_str());
  std::string features = GetFeatures();
  LOG_INFO("%s", features.c_str());

  // Init signal handler
  AppUtil::InitSignalHandler();

  // Run keydump if required
  if (isKeyDump)
  {
    Ui::RunKeyDump();
    return 0;
  }

  // Init app config
  AppConfig::Init();
  FileUtil::SetDownloadsDir(AppConfig::GetStr("downloads_dir"));
  static const bool isLogdumpEnabled = AppConfig::GetBool("logdump_enabled");

  // Init debug info, log last version
  DebugInfo::Init();
  const std::string versionUsed = DebugInfo::GetStr("version_used");
  if (!versionUsed.empty() && (versionUsed != AppUtil::GetAppVersion()))
  {
    LOG_INFO("last version %s", versionUsed.c_str());
  }

  // Init core dump
  static const bool isCoredumpEnabled = AppConfig::GetBool("coredump_enabled");
  if (isCoredumpEnabled)
  {
#ifndef HAS_COREDUMP
    LOG_WARNING("core dump not supported");
#else
    AppUtil::InitCoredump();
#endif
  }

  // Init message cache
  MessageCache::Init();

  // Run setup if required
  std::shared_ptr<Protocol> setupProtocol;
  if (isSetup)
  {
    setupProtocol = SetupProfile();
    if (!setupProtocol)
    {
      MessageCache::Cleanup();
      DebugInfo::Cleanup();
      AppConfig::Cleanup();
      return 1;
    }
  }

  // Init temp
  FileUtil::InitTempDir();

  // Init ui
  std::shared_ptr<Ui> ui = std::make_shared<Ui>();

  // Set message cache message handler
  std::function<void(std::shared_ptr<ServiceMessage>)> messageHandler =
    std::bind(&Ui::MessageHandler, std::ref(*ui), std::placeholders::_1);
  MessageCache::SetMessageHandler(messageHandler);

  // Load profile(s)
  std::string profilesDir = FileUtil::GetApplicationDir() + "/profiles";
  const std::vector<std::string>& profileNames = FileUtil::ListDirNames(profilesDir);
  std::vector<std::unique_ptr<ScopedDirLock>> profileDirLocks;
  for (auto& profileId : profileNames)
  {
    if (profileId == "version") continue;

    std::stringstream ss(profileId);
    std::string protocolName;
    if ((profileId.find("_") == std::string::npos) || !std::getline(ss, protocolName, '_'))
    {
      LOG_WARNING("invalid profile name, skipping %s", profileId.c_str());
      continue;
    }

#ifndef HAS_MULTIPROTOCOL
    if (!ui->GetProtocols().empty())
    {
      LOG_WARNING("multiple profile support not enabled, skipping %s", profileId.c_str());
      continue;
    }
#endif

    std::string profileDir = profilesDir + "/" + profileId;
    profileDirLocks.push_back(std::make_unique<ScopedDirLock>(profileDir));
    if (!profileDirLocks.back()->IsLocked())
    {
#ifdef HARBOR_BUILD
      HarborTrace::Os("profile.lock.failed", profileDirLocks.back()->Error(), {{"operation", profileDirLocks.back()->Operation()}});
#endif
      LOG_WARNING("unable to acquire lock for %s, skipping", profileDir.c_str());
      std::cerr << "warning: unable to acquire lock for " << profileDir << ", skipping.\n";
      profileDirLocks.pop_back();
      continue;
    }

    if (setupProtocol && (setupProtocol->GetProfileId() == profileId))
    {
      LOG_DEBUG("adding new profile %s", profileId.c_str());
      ui->AddProtocol(setupProtocol);
      setupProtocol.reset();
    }
    else
    {
      bool found = false;
      std::vector<std::shared_ptr<ProtocolBaseFactory>> allProtocolFactorys = GetProtocolFactorys();
      for (auto& protocolFactory : allProtocolFactorys)
      {
        if (protocolFactory->GetName() == protocolName)
        {
          LOG_DEBUG("loading existing profile %s", profileId.c_str());
          std::shared_ptr<Protocol> protocol = protocolFactory->Create();
          if (protocol)
          {
#ifdef HARBOR_BUILD
            HarborTrace::Event("profile.load.attempt", {{"protocol", protocolName}});
#endif
            const bool loaded = protocol->LoadProfile(profilesDir, profileId);
#ifdef HARBOR_BUILD
            HarborTrace::Event("profile.load.result", {{"protocol", protocolName}, {"loaded", loaded ? "yes" : "no"}});
#else
            (void)loaded;
#endif
            ui->AddProtocol(protocol);
            found = true;
          }
        }
      }

      if (!found)
      {
        std::string msg = "Protocol " + protocolName + " not supported";
        if (protocolName == "WhatsAppMd")
        {
          // Special logging for WhatsApp which may be auto-disabled from build if go dependency not met
          msg += " (requires Go >= " + std::string(GO_VERSION_MIN) + ")";
        }

        LOG_WARNING("%s", msg.c_str());
        std::cout << msg << "\n";
      }
    }
  }

  // Start protocol(s) and ui
#ifdef HARBOR_BUILD
  if (sessionFd < 0)
#endif
  ui->Init();
  std::unordered_map<std::string, std::shared_ptr<Protocol>> protocols = ui->GetProtocols();
  bool hasProtocols = !protocols.empty();
#ifdef HARBOR_BUILD
  if (sessionFd >= 0)
  {
    // Session backend: no UI; serve windows until none is connected.
    if (!hasProtocols)
    {
      HarborTrace::Event("profiles.none");
      HarborSession::FailBackend(sessionFd, "No profiles setup, exiting.");
    }
    else
    {
      std::map<std::string, std::shared_ptr<Protocol>> protocolsSorted(protocols.begin(), protocols.end());
      std::vector<std::shared_ptr<Protocol>> served;
      for (auto& protocol : protocolsSorted)
      {
        Status::Set(protocol.first, Status::FlagConnecting);
        served.push_back(protocol.second);
      }
      AppUtil::InitTerminateSignalHandler();
      HarborSession::RunBackend(sessionFd, FileUtil::GetApplicationDir(), served,
                                [](const std::function<void(std::shared_ptr<ServiceMessage>)>& handler)
                                { MessageCache::SetMessageHandler(handler); }, &sessionShutdownLock, [] { return AppUtil::IsTerminateRequested(); });
      for (auto& protocol : protocolsSorted)
      {
        protocol.second->Logout();
        protocol.second->CloseProfile();
      }
    }
  }
  else
#endif
  if (hasProtocols && exportDir.empty())
  {
    // Sort protocols
    std::map<std::string, std::shared_ptr<Protocol>> protocolsSorted(protocols.begin(), protocols.end());

    // Connecting status
    for (auto& protocol : protocolsSorted)
    {
      Status::Set(protocol.first, Status::FlagConnecting);
    }

    // Signal handler for terminate signals
    AppUtil::InitTerminateSignalHandler();

    // Login
    std::thread loginThread([&]
    {
      for (auto& protocol : protocolsSorted)
      {
        protocol.second->SetMessageHandler(messageHandler);
        protocol.second->Login();
      }
    });

    // Ui main loop
    ui->Run();

    // Cleanup login thread
    if (loginThread.joinable())
    {
      loginThread.join();
    }

    // Logout
    for (auto& protocol : protocolsSorted)
    {
      protocol.second->Logout();
      protocol.second->CloseProfile();
    }
  }

  // Clear cache message handler
  MessageCache::SetMessageHandler(nullptr);

  // Cleanup ui
  ui->Cleanup();
  ui.reset();

  // Perform export if requested
  if (!exportDir.empty())
  {
    MessageCache::Export(exportDir);
  }

  // Save last version
  DebugInfo::SetStr("version_used", AppUtil::GetAppVersion());

  // Cleanup
  FileUtil::CleanupTempDir();
  MessageCache::Cleanup();
  DebugInfo::Cleanup();
  AppConfig::Cleanup();
  Profiles::Cleanup();

  // Exit code
  int rv = 0;
  if (!hasProtocols)
  {
    std::cout << "No profiles setup, exiting.\n";
    rv = 1;
  }

  LOG_INFO("exit");

  Log::Cleanup(isLogdumpEnabled);

  if (AppUtil::IsTerminateRequested())
  {
    // Re-raise so the parent sees termination by signal (shell message, exit status 128+N)
    const int sig = AppUtil::GetTerminateSignal();
    signal(sig, SIG_DFL);
#ifdef HARBOR_BUILD
    HarborTrace::Event("process.termination", {{"signal", std::to_string(sig)}});
#endif
    kill(getpid(), sig);
  }

#ifdef HARBOR_BUILD
  if (!HarborUpdate::RestartTarget().empty() && !AppUtil::IsTerminateRequested())
  {
    const auto target = HarborUpdate::RestartTarget();
    const auto config = FileUtil::GetApplicationDir();
    protocols.clear();
    profileDirLocks.clear();
    dirLock.reset();
    if (harborDemo) execl(target.c_str(), target.c_str(), "--demo", "-d", config.c_str(), nullptr);
    else execl(target.c_str(), target.c_str(), "-d", config.c_str(), nullptr);
    std::cerr << "Could not restart Harbor. Your draft was saved; run harbor again.\n";
    return 1;
  }
#endif
#ifdef HARBOR_BUILD
  if (sessionShutdownLock >= 0)
  {
    protocols.clear();
    profileDirLocks.clear();
    dirLock.reset();
    close(sessionShutdownLock);
  }
#endif
  return rv;
}

std::string GetFeatures()
{
  std::vector<std::string> features;
#if defined(HAS_TELEGRAM)
  features.push_back("telegram");
#endif

#if defined(HAS_WHATSAPP)
  features.push_back("whatsapp");
#endif

#if defined(HAS_SIGNAL)
  features.push_back("signal");
#endif

  return StrUtil::Join(features, ", ");
}

void RemoveProfile()
{
  // Show profiles
  std::string profilesDir = FileUtil::GetApplicationDir() + "/profiles";
  const std::vector<std::string>& profileNames = FileUtil::ListDirNames(profilesDir);
  int id = 0;
  std::map<int, std::string> idPath;
  std::cout << "Remove profile:\n";
  for (auto& profileId : profileNames)
  {
    if (profileId == "version") continue;

    std::stringstream ss(profileId);
    std::string protocolName;
    if ((profileId.find("_") == std::string::npos) || !std::getline(ss, protocolName, '_'))
    {
      LOG_WARNING("invalid profile name, skipping %s", profileId.c_str());
      continue;
    }

    std::cout << id << ". " << profileId << "\n";
    idPath[id++] = profilesDir + "/" + profileId;
  }

  std::cout << id << ". Cancel removal\n";

  size_t selectid = id;
  std::cout << "Remove profile (" << selectid << "): ";
  std::string line;
  std::getline(std::cin, line);

  if (!line.empty())
  {
    try
    {
      selectid = stoi(line);
    }
    catch (...)
    {
    }
  }

  if (!idPath.count(selectid))
  {
    std::cout << "Removal aborted, exiting." << std::endl;
    return;
  }

  std::string profilePath = idPath.at(selectid);
  LOG_TRACE("deleting %s", profilePath.c_str());
  FileUtil::RmDir(profilePath);
}

std::shared_ptr<Protocol> SetupProfile()
{
  std::shared_ptr<Protocol> rv;
  std::vector<std::shared_ptr<ProtocolBaseFactory>> protocolFactorys = GetProtocolFactorys();

  std::cout << "Protocols:" << std::endl;
  size_t idx = 0;
  for (auto it = protocolFactorys.begin(); it != protocolFactorys.end(); ++it, ++idx)
  {
    std::cout << idx << ". " << (*it)->GetName() << std::endl;
  }
  std::cout << idx << ". Exit setup" << std::endl;

  size_t selectidx = idx;
  std::cout << "Select protocol (" << selectidx << "): ";
  std::string line;
  std::getline(std::cin, line);

  if (!line.empty())
  {
    try
    {
      selectidx = stoi(line);
    }
    catch (...)
    {
    }
  }

  if (selectidx >= protocolFactorys.size())
  {
    std::cout << "Setup aborted, exiting." << std::endl;
    return rv;
  }

  std::string profileId;
  std::string profilesDir = FileUtil::GetApplicationDir() + std::string("/profiles");

#ifndef HAS_MULTIPROTOCOL
  FileUtil::RmDir(profilesDir);
  FileUtil::MkDir(profilesDir);
  Profiles::Init();
#endif

  std::string setupMessage = protocolFactorys.at(selectidx)->GetSetupMessage();
  if (!setupMessage.empty())
  {
    LOG_WARNING("%s", setupMessage.c_str());
    std::cout << setupMessage;
    sleep(3);
  }

  std::shared_ptr<Protocol> protocol = protocolFactorys.at(selectidx)->Create();
  bool setupResult = protocol && protocol->SetupProfile(profilesDir, profileId);
  if (setupResult)
  {
    std::cout << "Succesfully set up profile " << profileId << "\n";
    rv = protocol;
  }
  else
  {
    std::cout << "Setup failed\n";
    if (protocol)
    {
      protocol->Logout();
      protocol->CloseProfile();
    }
  }

  return rv;
}

void ShowHelp()
{
#ifdef HARBOR_BUILD
  std::cout << "Harbor — a Telegram TUI built on nchat\n\n"
    "Usage: harbor [--demo | -s | -d DIR | --version | --check-session]\n\n"
    "  --demo     isolated offline demo account\n"
    "  --check-session   read-only startup check for the selected -d/--demo directory\n"
    "                    exit 0 if absent or compatible; 1 with the session error otherwise\n"
    "                    two-second connect/welcome limit; no backend start or config writes\n"
    "  --open-telegram-chat ID   open this signed decimal chat ID in this window\n"
    "  --open-telegram-topic ID  select this positive decimal forum topic in that chat\n"
    "  --open-profile ID        select the exact native Telegram account profile\n"
    "  --target-surface ID --target-pid PID   route only that existing UI; never launch another\n"
    "  --focus-guard           hold Focus/Preview ordering through one framed osascript effect\n"
    "  --inspect-ui            read current conversation identity of the exact targeted UI\n"
    "  --inspect-window        read native window generation, loaded build and opaque conversation identity\n"
    "  --park-ui TOKEN         return an unchanged duplicate UI to Chats, retaining its drafts\n"
    "  -s         set up your Telegram account\n"
    "  -d DIR     use a separate configuration directory\n"
    "  -k         inspect terminal key sequences\n"
    "  Ctrl+Q     quit\n\n"
    "Type to compose. Enter sends; Shift+Enter inserts a newline.\n"
    "Esc browses messages. Arrows select; Enter opens actions.\n"
    "Option+K (Esc k) or Ctrl+K finds chats. / opens commands from an empty composer.\n"
    "Ctrl+G jumps to latest messages; Ctrl+R refreshes the conversation.\n"
    "Config defaults to ~/.config/harbor; stock nchat is unchanged.\n";
  return;
#endif
  std::cout <<
    "nchat is a terminal-based messaging client with Telegram, WhatsApp and Signal support.\n"
    "\n"
    "Usage: nchat [OPTION]\n"
    "\n"
    "Command-line Options:\n"
    "    -d, --confdir <DIR>    use a different directory than ~/.config/nchat\n"
    "    -e, --verbose          enable verbose logging\n"
    "    -ee, --extra-verbose   enable extra verbose logging\n"
    "    -h, --help             display this help and exit\n"
    "    -k, --keydump          key code dump mode\n"
    "    -m, --devmode          developer mode\n"
    "    -r, --remove           remove chat protocol account\n"
    "    -s, --setup            set up chat protocol account\n"
    "    -v, --version          output version information and exit\n"
    "    -x, --export <DIR>     export message cache to specified dir\n"
    "\n"
    "Interactive Commands:\n"
    "    PageDn      history next page\n"
    "    PageUp      history previous page\n"
    "    Tab         next chat\n"
    "    Sh-Tab      previous chat\n"
    "    Ctrl-f      jump to unread chat\n"
    "    Ctrl-g      toggle show help bar\n"
    "    Ctrl-l      toggle show contact list\n"
    "    Ctrl-n      goto chat\n"
    "    Ctrl-p      toggle show top bar\n"
    "    Ctrl-q      quit\n"
    "    Ctrl-s      insert emoji\n"
    "    Ctrl-t      send file\n"
    "    Ctrl-x      send message\n"
    "    Ctrl-y      toggle show emojis\n"
    "    KeyUp       select message\n"
    "    Alt-@       insert mention\n"
    "    Alt-a       archive current chat\n"
    "    Alt-d       delete/leave current chat\n"
    "    Alt-e       external editor compose\n"
    "    Alt-i       auto-compose reply\n"
    "    Alt-n       search contacts\n"
    "    Alt-p       pin/unpin current chat\n"
    "    Alt-t       external telephone call\n"
    "    Alt-/       find in chat\n"
    "    Alt-?       find next in chat\n"
    "    Alt-$       external spell check\n"
    "    Alt-,       decrease contact list width\n"
    "    Alt-.       increase contact list width\n"
    "\n"
    "Interactive Commands for Selected Message:\n"
    "    Ctrl-d      delete selected message\n"
    "    Ctrl-r      download attached file\n"
    "    Ctrl-v      open/view attached file\n"
    "    Ctrl-w      open link\n"
    "    Ctrl-x      send reply to selected message\n"
    "    Ctrl-z      edit selected message\n"
    "    Alt-c       copy selected message to clipboard\n"
    "    Alt-q       jump to quoted/replied message\n"
    "    Alt-r       forward selected message\n"
    "    Alt-s       add/remove reaction on selected message\n"
    "    Alt-w       external message viewer\n"
    "\n"
    "Interactive Commands for Text Input:\n"
    "    Ctrl-a      move cursor to start of line\n"
    "    Ctrl-c      clear input buffer\n"
    "    Ctrl-e      move cursor to end of line\n"
    "    Ctrl-k      delete from cursor to end of line\n"
    "    Ctrl-u      delete from cursor to start of line\n"
    "    Alt-Left    move cursor backward one word\n"
    "    Alt-Right   move cursor forward one word\n"
    "    Alt-Backsp  delete previous word\n"
    "    Alt-Delete  delete next word\n"
    "    Alt-c       copy input buffer to clipboard (if no message selected)\n"
    "    Alt-v       paste into input buffer from clipboard\n"
    "    Alt-x       cut input buffer to clipboard\n"
    "\n"
    "Report bugs at https://github.com/d99kris/nchat\n"
    "\n";
}

void ShowVersion()
{
#ifdef HARBOR_BUILD
  std::cout << "Harbor " << HarborUpdate::Version() << " — native interface; backend: ";
#endif
  // *INDENT-OFF*
  std::cout <<
    AppUtil::GetAppName(true /*p_WithVersion*/) << "\n"
    "\n"
    "Copyright (c) 2019-2026 Kristofer Berggren\n"
    "\n"
#if defined(HAS_SIGNAL)
    "Combined distribution subject to GNU AGPL v3 license.\n"
#elif defined(HAS_WHATSAPP)
    "Combined distribution subject to GNU GPL v3 license.\n"
#else
    "Combined distribution subject to MIT license.\n"
#endif
    "\n"
    "Written by Kristofer Berggren.\n";
  // *INDENT-ON*
}

#ifdef HARBOR_BUILD
// One Harbor window: the ordinary UI, attached to the directory's shared session backend. It
// skips the native initialisers and cleanup (log, config, cache, temp, profiles) that only the
// backend may run.
static int RunHarborWindow(const std::vector<std::string>& p_BackendArgs, const HarborRoute::Target& p_Target)
{
  const std::string appDir = FileUtil::GetApplicationDir();
  AppUtil::InitTerminateSignalHandler();
  if (!HarborRestart::WaitForBackend(appDir))
  {
    std::cerr << "Harbor update did not complete. Your saved chat and draft are retained; run Harbor again.\n";
    return 1;
  }
  HarborSession::Attachment attachment = HarborSession::Attach(appDir, FileUtil::GetSelfPath(), p_BackendArgs);
  if (attachment.fd < 0)
  {
    HarborTrace::Event("window.attach.failed");
    std::cerr << attachment.error << "\n";
    return 1;
  }
  if (attachment.profiles.empty())
  {
    close(attachment.fd);
    std::cout << "No profiles setup, exiting.\n";
    return 1;
  }

  std::string initialProfile;
  if (p_Target.Present())
  {
    std::vector<std::string> profiles;
    for (const auto& profile : attachment.profiles) profiles.push_back(profile.id);
    try { initialProfile = HarborRoute::SelectProfile(p_Target, profiles, harborDemo); }
    catch (const std::exception& error) { close(attachment.fd); std::cerr << error.what() << '\n'; return 1; }
  }
  const int slot = HarborSession::AcquireSlot(appDir);
  if (slot < 0)
  {
    close(attachment.fd);
    std::cerr << "Could not reserve an independent Harbor view state in " << appDir << ".\n";
    return 1;
  }
  HarborTrace::Event("window.ready", {{"slot", std::to_string(slot)}, {"profiles", std::to_string(attachment.profiles.size())}});
  auto connection = std::make_shared<HarborSession::Connection>(attachment.fd, attachment.pending, attachment.connectionId);
  std::shared_ptr<Ui> ui = std::make_shared<Ui>();
  ui->SetSession(slot, [connection] { connection->Lent(); }, attachment.generation);
  if (p_Target.Present()) ui->SetInitialChat(initialProfile, p_Target.conversation);
  std::function<void(std::shared_ptr<ServiceMessage>)> messageHandler =
    std::bind(&Ui::MessageHandler, std::ref(*ui), std::placeholders::_1);
  std::vector<std::shared_ptr<Protocol>> remotes;
  for (auto& profile : attachment.profiles)
  {
    auto remote = std::make_shared<HarborSession::Remote>(connection, profile);
    remote->SetMessageHandler(messageHandler);
    ui->AddProtocol(remote);
    remotes.push_back(remote);
  }

  Clipboard::LoadCommands(appDir + "/app.conf");
  AppUtil::InitTerminateSignalHandler();
  ui->Init();
  for (auto& remote : remotes) remote->Login(); // starts the shared reader once
  ui->Run();
  // Freeze the reader and recover unconfirmed sends before Cleanup drains events
  // and writes this slot. This also covers restart and caught termination signals.
  connection->Stop();
  ui->Cleanup();
  const bool ended = connection->Ended();
  const std::string reason = connection->EndReason();
  ui.reset();
  remotes.clear();

  if (ended)
    std::cerr << reason << "\n";

  if (AppUtil::IsTerminateRequested())
  {
    const int sig = AppUtil::GetTerminateSignal();
    signal(sig, SIG_DFL);
#ifdef HARBOR_BUILD
    HarborTrace::Event("process.termination", {{"signal", std::to_string(sig)}});
#endif
    kill(getpid(), sig);
  }

  if (!HarborUpdate::RestartTarget().empty() && !AppUtil::IsTerminateRequested() && !ended)
  {
    // Restart only this window; it reattaches, or starts a fresh backend if it was the last one.
    const auto target = HarborUpdate::RestartTarget();
    std::vector<std::string> args = { target };
    args.insert(args.end(), p_BackendArgs.begin(), p_BackendArgs.end());
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    HarborTrace::Event("process.restart.attempt", {{"executable", target}});
    execv(target.c_str(), argv.data());
    HarborTrace::Os("process.restart.failed", errno);
    std::cerr << "Could not restart Harbor. Your draft was saved; run harbor again.\n";
    return 1;
  }
  return ended ? 1 : 0;
}
#endif

// Keep entry/exit observation outside every early return, including help and failed startup.
int main(int argc, char* argv[])
{
#ifdef HARBOR_BUILD
  const bool sessionProbe = std::any_of(argv + 1, argv + argc, [](const char* arg) { return std::string(arg) == "--check-session"; });
  const bool focusHelper = std::any_of(argv + 1, argv + argc, [](const char* arg) { return std::string(arg) == "--focus-guard" || std::string(arg) == "--focus-guard-effect"; });
  if (sessionProbe && focusHelper)
  { std::cerr << "--check-session applies only to ordinary Harbor startup arguments; Focus helper modes cannot be combined with it.\n"; return 1; }
  if (argc > 1 && std::string(argv[1]) == "--focus-guard-effect") return HarborPreviewGuard::Effect(argc, argv);
  if (std::any_of(argv + 1, argv + argc, [](const char* arg) { return std::string(arg) == "--focus-guard"; }))
  {
    if (argc != 2) { std::cerr << "Focus guard takes no arguments; invoke harbor --focus-guard and send the framed request on stdin.\n"; return 2; }
    return HarborPreviewGuard::Run();
  }
  // A check must not create even the operational-log directories before parsing its args.
  const bool checking = std::any_of(argv + 1, argv + argc, [](const char* arg) { return std::string(arg) == "--check-session" || std::string(arg) == "--inspect-window"; });
  if (!checking)
  {
    HarborTrace::Start(FileUtil::GetSelfPath(), HarborUpdate::Version(), SysUtil::GetBuildInfo(), SysUtil::GetBuildGitSha());
    AppUtil::SetSignalObserver(HarborTrace::Signal);
  }
#endif
  const int result = ApplicationMain(argc, argv);
#ifdef HARBOR_BUILD
  HarborTrace::Event("process.exit", {{"status", std::to_string(result)}});
#endif
  return result;
}
