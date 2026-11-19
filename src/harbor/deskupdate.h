#pragma once
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace HarborDeskUpdate
{
constexpr int64_t IdleMS = 120000;
inline int64_t Now()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::steady_clock::now().time_since_epoch()).count();
}
struct State
{
  bool menu = false, child = false, routing = false, attachments = false, mouse = false,
       browse = false, selection = false, auth = false, latest = false, draftsEmpty = false,
       stable = false;
};
inline bool Ready(int64_t now, int64_t lastInput, const State& state)
{
  return lastInput >= 0 && now >= lastInput && now - lastInput >= IdleMS &&
    !state.menu && !state.child && !state.routing && !state.attachments && !state.mouse &&
    !state.browse && !state.selection && !state.auth && state.latest && state.draftsEmpty && state.stable;
}
inline std::string Path()
{
  const char* value = std::getenv("HARBOR_DESK_UPDATE_FILE");
  return value ? value : "";
}
inline std::string Request(const std::string& path, pid_t owner, int64_t wallNow)
{
  if (path.empty() || owner <= 1) return {};
  const int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) return {};
  struct stat info{};
  std::array<char, 257> buffer{};
  const bool regular = fstat(fd, &info) == 0 && S_ISREG(info.st_mode) &&
    info.st_uid == geteuid() && !(info.st_mode & 077) && info.st_size > 0 && info.st_size <= 256;
  size_t used = 0;
  bool complete = false;
  if (regular)
    while (used < buffer.size())
    {
      const auto count = read(fd, buffer.data() + used, buffer.size() - used);
      if (count == 0) { complete = true; break; }
      if (count < 0) break;
      used += size_t(count);
    }
  close(fd);
  if (!complete || used > 256) return {};
#ifdef __APPLE__
  const auto modified = info.st_mtimespec;
#else
  const auto modified = info.st_mtim;
#endif
  if (int64_t(modified.tv_sec) * 1000 + modified.tv_nsec / 1000000 > wallNow) return {};
  const std::string data(buffer.data(), used);
  const std::string prefix = "pcl.desk.update.v1\t" + std::to_string(owner) + '\t';
  if (data.size() != prefix.size() + 41 || data.compare(0, prefix.size(), prefix) || data.back() != '\n') return {};
  const auto revision = data.substr(prefix.size(), 40);
  for (auto ch : revision)
    if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) return {};
  return revision;
}
inline bool Acknowledge(const std::string& path, pid_t owner, const std::string& revision, int64_t wallNow)
{
  if (owner != getppid() || Request(path, owner, wallNow) != revision) return false;
  const std::string temp = path + ".ack." + std::to_string(getpid());
  const int fd = open(temp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (fd < 0) return false;
  const std::string data = "pcl.desk.updated.v1\t" + std::to_string(owner) + '\t' + revision + '\n';
  const bool written = write(fd, data.data(), data.size()) == ssize_t(data.size()) && fsync(fd) == 0;
  const bool closed = close(fd) == 0;
  const bool ok = written && closed && rename(temp.c_str(), path.c_str()) == 0;
  if (!ok) unlink(temp.c_str());
  return ok;
}
}
