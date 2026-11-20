#pragma once
// A local backend lifetime identity. No session wire fields change: older backends have no
// live publisher. The advisory lock makes stale records unusable, even after inode/PID reuse.
#include <array>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <string>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace HarborSessionGeneration
{
inline std::string SocketIdentity(const struct stat& info)
{
  return std::to_string(info.st_dev) + ":" + std::to_string(info.st_ino);
}
inline bool Valid(const std::string& token)
{
  return token.size() == 32 && token.find_first_not_of("0123456789abcdef") == std::string::npos;
}
inline bool PrivateFile(const struct stat& info)
{
  return S_ISREG(info.st_mode) && info.st_uid == geteuid() && !(info.st_mode & 077) && info.st_nlink == 1;
}
inline std::string Path(const std::string& appDir) { return appDir + "/session.generation"; }

class Publisher
{
public:
  Publisher() = default;
  Publisher(const Publisher&) = delete;
  Publisher& operator=(const Publisher&) = delete;
  ~Publisher()
  {
    if (m_Fd < 0) return;
    struct stat held{}, current{};
    if (fstat(m_Fd, &held) == 0 && lstat(m_Path.c_str(), &current) == 0 &&
        held.st_dev == current.st_dev && held.st_ino == current.st_ino) unlink(m_Path.c_str());
    close(m_Fd);
  }
  bool Publish(const std::string& appDir, const std::string& socketPath)
  {
    struct stat socket{}, previous{};
    if (lstat(socketPath.c_str(), &socket) || !S_ISSOCK(socket.st_mode) || socket.st_uid != geteuid()) return false;
    if (lstat(Path(appDir).c_str(), &previous) == 0)
    {
      if (!PrivateFile(previous)) return false;
      const int prior = open(Path(appDir).c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW);
      if (prior < 0) return false;
      const bool inactive = flock(prior, LOCK_SH | LOCK_NB) == 0;
      close(prior);
      if (!inactive) return false;
    }
    else if (errno != ENOENT) return false;
    std::array<unsigned char, 16> bytes{};
    int random = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (random < 0) return false;
    const auto count = read(random, bytes.data(), bytes.size());
    close(random);
    if (count != ssize_t(bytes.size())) return false;
    const char* hex = "0123456789abcdef";
    std::string token;
    for (auto value : bytes) { token += hex[value >> 4]; token += hex[value & 15]; }
    const auto temporary = Path(appDir) + "." + token;
    int fd = open(temporary.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return false;
    const auto data = "pcl.harbor.session-generation.v1\t" + token + "\t" + SocketIdentity(socket) + "\n";
    // Publish immutable bytes atomically; a reader never sees a generation being rewritten.
    const bool ok = flock(fd, LOCK_EX | LOCK_NB) == 0 &&
      write(fd, data.data(), data.size()) == ssize_t(data.size()) && rename(temporary.c_str(), Path(appDir).c_str()) == 0;
    if (!ok) { close(fd); unlink(temporary.c_str()); return false; }
    m_Fd = fd;
    m_Path = Path(appDir);
    return true;
  }
private:
  int m_Fd = -1;
  std::string m_Path;
};

// Empty token means an older backend or an inactive stale record. A live publisher's record
// must be private, valid and bound to the endpoint being examined. No connection is opened.
inline bool Read(const std::string& appDir, const struct stat& socket, std::string& token)
{
  token.clear();
  const int fd = open(Path(appDir).c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW);
  if (fd < 0) return errno == ENOENT;
  struct stat file{};
  if (fstat(fd, &file) || !PrivateFile(file)) { close(fd); return false; }
  if (flock(fd, LOCK_SH | LOCK_NB) == 0) { close(fd); return true; }
  if (errno != EWOULDBLOCK && errno != EAGAIN) { close(fd); return false; }
  std::array<char, 257> bytes{};
  const auto count = read(fd, bytes.data(), bytes.size());
  close(fd);
  if (count <= 0 || count != file.st_size || count > 256) return false;
  const std::string data(bytes.data(), count), prefix = "pcl.harbor.session-generation.v1\t";
  if (data.compare(0, prefix.size(), prefix)) return false;
  const auto end = data.find('\t', prefix.size());
  if (end == std::string::npos) return false;
  const auto current = data.substr(prefix.size(), end - prefix.size());
  if (!Valid(current)) return false;
  if (data.substr(end + 1) != SocketIdentity(socket) + "\n") return true;
  token = current;
  return true;
}
}
