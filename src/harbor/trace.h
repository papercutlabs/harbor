#pragma once
// Operational metadata only. Callers must never supply message text, raw argv, prompts,
// authentication replies, or provider payloads. Logging is optional and preserves errno.
#include <cerrno>
#include <cstdio>
#include <cstdint>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <vector>
#include <string>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace HarborTrace
{
using Fields = std::vector<std::pair<std::string, std::string>>;
struct State
{
  std::string path, run, startup, signalPrefix, version, build, revision, started, surface, launchWorkspace;
  std::terminate_handler previous = nullptr;
};
inline State& Get() { static State state; return state; }
inline std::string Number(uint64_t n) { return std::to_string(n); }
inline bool Uuid(const std::string& s)
{
  if (s.size() != 36) return false;
  for (size_t i = 0; i < s.size(); ++i)
    if (i == 8 || i == 13 || i == 18 || i == 23) { if (s[i] != '-') return false; }
    else if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f') || (s[i] >= 'A' && s[i] <= 'F'))) return false;
  return true;
}
inline std::string Quote(const std::string& value)
{
  std::string out = "\"";
  // Bounded metadata, including filenames, cannot inject new records.
  for (unsigned char c : value.substr(0, 1024))
  {
    if (c == '"' || c == '\\') { out += '\\'; out += char(c); }
    else if (c >= 32 && c < 127) out += char(c);
    else { const char* hex = "0123456789abcdef"; out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15]; }
  }
  return out + '"';
}
inline std::string Token(const std::string& value)
{
  // A directory identity avoids persisting a user-supplied confdir or account name.
  uint64_t h = 14695981039346656037ULL;
  for (unsigned char c : value) { h ^= c; h *= 1099511628211ULL; }
  return Number(h);
}
inline int Open(const std::string& path)
{
  const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0) return -1;
  struct stat st = {};
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 077) != 0)
  { close(fd); return -1; }
  return fd;
}
inline void Event(const std::string& name, Fields fields = {}) noexcept
{
  const int saved = errno;
  try
  {
    auto& s = Get();
    if (!s.path.empty())
    {
      const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
      std::string line = "{\"schema\":1,\"ts_ms\":" + Number(ms) + ",\"component\":\"harbor\",\"pid\":" + Number(getpid()) +
        ",\"ppid\":" + Number(getppid()) + ",\"run_id\":" + Quote(s.run) + ",\"startup_id\":" + Quote(s.startup) + ",\"event\":" + Quote(name) +
        ",\"started_ms\":" + Quote(s.started) + ",\"loaded_version\":" + Quote(s.version) + ",\"loaded_build\":" + Quote(s.build) + ",\"loaded_revision\":" + Quote(s.revision) +
        ",\"surface\":" + Quote(s.surface) + ",\"launch_workspace\":" + Quote(s.launchWorkspace);
      for (const auto& f : fields) line += "," + Quote(f.first) + ":" + Quote(f.second);
      line += "}\n";
      int fd = Open(s.path);
      if (fd >= 0)
      {
        // Never wait for another writer. Rotation is metadata-only, independent of native locks.
        if (flock(fd, LOCK_EX | LOCK_NB) == 0)
        {
          struct stat st = {};
          if (fstat(fd, &st) == 0 && st.st_size > 5 * 1024 * 1024)
          {
            if (rename(s.path.c_str(), (s.path + ".1").c_str()) == 0)
            { close(fd); fd = Open(s.path); }
          }
          if (fd >= 0) (void)!write(fd, line.data(), line.size());
        }
        if (fd >= 0) close(fd);
      }
    }
  }
  catch (...) { } // diagnostics must never change the operation's outcome
  errno = saved;
}
inline void Os(const std::string& operation, int error, Fields fields = {}) noexcept
{
  // strerror is the original OS diagnostic, never a reconstructed owner claim.
  fields.emplace_back("errno", std::to_string(error));
  fields.emplace_back("diagnostic", std::strerror(error));
  Event(operation, fields);
}
inline void Signal(int sig) noexcept
{
  const int saved = errno;
  auto& s = Get();
  const int fd = Open(s.path);
  if (fd >= 0)
  {
    // Prebuilt prefix, stack-only integer formatting and async-signal-safe file syscalls.
    // Open the current file so rotation by another process cannot orphan the record.
    char line[512]; size_t n = 0;
    for (char c : s.signalPrefix) if (n < sizeof(line) - 32) line[n++] = c;
    char digits[16]; size_t count = 0; unsigned int value = sig;
    do { digits[count++] = char('0' + value % 10); value /= 10; } while (value && count < sizeof(digits));
    while (count) line[n++] = digits[--count];
    line[n++] = '}'; line[n++] = '\n';
    (void)!write(fd, line, n);
    close(fd);
  }
  errno = saved;
}
inline void EarlySignal(int sig)
{
  Signal(sig);
  signal(sig, SIG_DFL);
  kill(getpid(), sig);
}
inline void Start(const std::string& executable, const std::string& version, const std::string& build, const std::string& revision = {}) noexcept
{
  const int saved = errno;
  try
  {
    auto& s = Get();
    const char* home = getenv("HOME");
    if (!home || !*home) { errno = saved; return; }
    // Same private local log home for bare Harbor and Desk-launched Harbor. No profile is touched.
    const char* stateHome = getenv("XDG_STATE_HOME");
    std::string dir;
    if (stateHome && *stateHome && *stateHome == '/') { dir = stateHome; mkdir(dir.c_str(), 0700); }
    else { dir = std::string(home) + "/.local"; mkdir(dir.c_str(), 0700); dir += "/state"; mkdir(dir.c_str(), 0700); }
    dir += "/harbor"; mkdir(dir.c_str(), 0700);
    s.path = dir + "/operational.jsonl";
    const auto clock = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    s.run = Number(getpid()) + "-" + Number(clock);
    s.started = Number(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    s.version = version; s.build = build; s.revision = revision;
    const char* surface = getenv("CMUX_SURFACE_ID");
    const char* workspace = getenv("CMUX_WORKSPACE_ID");
    s.surface = surface && Uuid(surface) ? surface : "";
    s.launchWorkspace = workspace && Uuid(workspace) ? workspace : "";
    const char* startup = getenv("OFFICE_DESK_STARTUP_ID");
    s.startup = startup && Uuid(startup) ? startup : "";
    if (s.startup.empty())
    {
      unsigned char bytes[16] = {};
      const int random = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
      const bool haveRandom = random >= 0 && read(random, bytes, sizeof(bytes)) == sizeof(bytes);
      if (random >= 0) close(random);
      if (!haveRandom) for (size_t i = 0; i < sizeof(bytes); ++i) bytes[i] = (unsigned char)((uint64_t(clock) ^ uint64_t(getpid())) >> ((i % 8) * 8));
      bytes[6] = (bytes[6] & 15) | 64; bytes[8] = (bytes[8] & 63) | 128;
      const char* hex = "0123456789abcdef";
      for (size_t i = 0; i < sizeof(bytes); ++i)
      { if (i == 4 || i == 6 || i == 8 || i == 10) s.startup += '-'; s.startup += hex[bytes[i] >> 4]; s.startup += hex[bytes[i] & 15]; }
      setenv("OFFICE_DESK_STARTUP_ID", s.startup.c_str(), 1);
    }
    s.signalPrefix = "{\"schema\":1,\"component\":\"harbor\",\"pid\":" + Number(getpid()) + ",\"run_id\":" + Quote(s.run) +
      ",\"startup_id\":" + Quote(s.startup) + ",\"event\":\"process.signal\",\"signal\":";
    for (int sig : { SIGABRT, SIGBUS, SIGFPE, SIGILL, SIGQUIT, SIGSEGV, SIGSYS, SIGTRAP })
    {
      struct sigaction previous = {};
      if (sigaction(sig, nullptr, &previous) == 0 && previous.sa_handler == SIG_DFL)
      {
        struct sigaction action = {};
        action.sa_handler = EarlySignal; sigemptyset(&action.sa_mask);
        sigaction(sig, &action, nullptr);
      }
    }
    s.previous = std::set_terminate([] {
      Event("process.uncaught_exception");
      if (Get().previous) Get().previous();
      std::abort();
    });
    Event("process.entry", {{"executable", executable}, {"version", version}, {"build", build},
      {"stdin_tty", isatty(0) ? "yes" : "no"}, {"stdout_tty", isatty(1) ? "yes" : "no"}});
  }
  catch (...) { }
  errno = saved;
}
}
