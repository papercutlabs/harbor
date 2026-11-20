#include "harbor.h"
#include "fileutil.h"
#include "strutil.h"
#include "update.h"
#include "trace.h"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace
{
std::string Directory()
{
  const char* home = getenv("HOME");
  if (!home || !*home) return {};
  return std::string(home) + "/.office/harbor-windows";
}
bool PrivateDirectory(const std::string& path)
{
  struct stat info{};
  return lstat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode) && info.st_uid == geteuid() && !(info.st_mode & 077);
}
std::string RandomId()
{
  std::array<unsigned char, 16> bytes{};
  const int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
  if (fd < 0) return {};
  const auto count = read(fd, bytes.data(), bytes.size());
  close(fd);
  if (count != ssize_t(bytes.size())) return {};
  const char* hex = "0123456789abcdef";
  std::string id;
  for (auto value : bytes) { id += hex[value >> 4]; id += hex[value & 15]; }
  return id;
}
}
void Harbor::RegisterRestart()
{
  const char* home = getenv("HOME");
  if (!home || !*home) return;
  const auto parent = std::string(home) + "/.office";
  mkdir(parent.c_str(), 0700);
  // Existing Office configuration may be readable; only this dedicated registry must be private.
  struct stat info{};
  if (lstat(parent.c_str(), &info) || !S_ISDIR(info.st_mode) || info.st_uid != geteuid() || (info.st_mode & 022)) return;
  const auto directory = Directory();
  mkdir(directory.c_str(), 0700);
  if (!PrivateDirectory(directory)) return;
  const auto id = RandomId();
  if (id.empty()) return;
  m_RestartRegistration = directory + "/" + std::to_string(getpid()) + "-" + id;
  const int fd = open(m_RestartRegistration.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0) { m_RestartRegistration.clear(); return; }
  const auto data = "pcl.harbor.window.v1\t" + std::to_string(getpid()) + "\t" + HarborUpdate::Version() + "\t" + StrUtil::StrToHex(FileUtil::GetSelfPath()) + "\n";
  const bool ok = write(fd, data.data(), data.size()) == ssize_t(data.size());
  close(fd);
  if (!ok) UnregisterRestart();
}
void Harbor::UnregisterRestart()
{
  if (m_RestartRegistration.empty()) return;
  unlink((m_RestartRegistration + ".restart").c_str());
  unlink(m_RestartRegistration.c_str());
  m_RestartRegistration.clear();
}
void Harbor::TryRestartAll()
{
  if (m_RestartRegistration.empty() || m_InDeskView || m_InColdDeskView || m_AuthSuspended || !m_AuthOwner.empty() ||
      m_ControlPending || m_ControlReply >= 0 || m_InitialRouteUntil || !m_RestoreChat.empty() || m_MouseDown) return;
  const auto path = m_RestartRegistration + ".restart";
  const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW);
  if (fd < 0) return;
  struct stat info{};
  std::array<char, 8193> bytes{};
  ssize_t count = -1;
  if (fstat(fd, &info) == 0 && S_ISREG(info.st_mode) && info.st_uid == geteuid() && !(info.st_mode & 077) && info.st_size > 0 && info.st_size <= 8192)
    count = read(fd, bytes.data(), bytes.size());
  close(fd);
  if (count <= 0 || count != info.st_size) return;
  const std::string data(bytes.data(), count), prefix = "pcl.harbor.restart.v1\t";
  if (data.compare(0, prefix.size(), prefix) || data.back() != '\n') return;
  const auto target = data.substr(prefix.size(), data.size() - prefix.size() - 1);
  if (target.empty() || target[0] != '/' || target.find_first_of("\r\n\t") != std::string::npos || access(target.c_str(), X_OK)) return;
  struct stat executable{};
  if (stat(target.c_str(), &executable) || !S_ISREG(executable.st_mode) || executable.st_uid != geteuid() || (executable.st_mode & 022)) return;
  // Numeric slots can be reacquired in a different order when several UIs exec together.
  // A private identity resumes this window's own state irrespective of the next numeric slot.
  if (m_RestartStateId.empty()) m_RestartStateId = RandomId();
  if (m_RestartStateId.empty() || !SaveState(true)) { Notify("Update waiting: this window could not save its state"); return; }
  setenv("HARBOR_RESTART_STATE_ID", m_RestartStateId.c_str(), 1);
  HarborTrace::Event("ui.restart.all", {{"executable", target}});
  HarborUpdate::RestartTarget() = target;
  m_Running = false;
}
void Harbor::UpdateAll()
{
  // Reuse Office's installed argv even when the running Desk still writes an older menu.
  // Fixed argv, never a shell command; the existing Desk entry identifies its own Office.
  const char* configured = getenv("HARBOR_DESK_COMMANDS");
  const char* home = getenv("HOME");
  const std::string path = configured && *configured ? configured : home ? std::string(home) + "/.office/desk/harbor-desk-commands" : "";
  std::ifstream input(path);
  std::string line;
  while (std::getline(input, line))
  {
    std::vector<std::string> fields;
    std::istringstream row(line);
    std::string field;
    while (std::getline(row, field, '\t')) fields.push_back(field);
    if (fields.size() >= 7 && !fields[2].empty() && !fields[3].empty() && fields[4] == "desk" && fields[5] == "--view")
    {
      CloseMenu();
      DeskHandoff({fields[2], fields[3], "update", "--restart-harbor"});
      return;
    }
  }
  Notify("Start office desk once to enable Harbor updates");
}
