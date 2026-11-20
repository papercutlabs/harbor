#pragma once
#include "launchroute.h"
#include <cerrno>
#include <chrono>
#include <cctype>
#include <fcntl.h>
#include <functional>
#include <poll.h>
#include <sstream>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
namespace HarborUiControl
{
inline bool Field(const std::string& s)
{ return s.size() <= 256 && std::all_of(s.begin(), s.end(), [](unsigned char c) { return c >= 32 && c <= 126 && c != '\t'; }); }
inline std::string Surface(std::string s)
{
  if (s.empty() || s.size() > 128 || !std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isalnum(c) || c == '-' || c == '_' || c == ':'; }))
    throw std::invalid_argument("Target Harbor surface must be its exact cmux surface ID.");
  for (auto& c : s) c = std::toupper(static_cast<unsigned char>(c));
  return s;
}
inline std::string Path(const std::string& dir, const std::string& surface)
{
  uint64_t hash = 14695981039346656037ULL;
  for (unsigned char c : surface) { hash ^= c; hash *= 1099511628211ULL; }
  std::ostringstream name; name << dir << "/ui-" << std::hex << hash << ".sock";
  return name.str();
}
inline void NoSigPipe(int fd)
{
#ifdef SO_NOSIGPIPE
  int one = 1; setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#else
  (void)fd;
#endif
}
inline bool SameUser(int fd)
{
#if defined(__APPLE__) || defined(__FreeBSD__)
  uid_t uid; gid_t gid;
  return getpeereid(fd, &uid, &gid) == 0 && uid == geteuid();
#else
  struct ucred peer; socklen_t len = sizeof(peer);
  return getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &len) == 0 && peer.uid == geteuid();
#endif
}
inline bool Address(const std::string& path, sockaddr_un& addr)
{
  if (path.size() >= sizeof(addr.sun_path)) return false;
  addr = {}; addr.sun_family = AF_UNIX;
  std::copy(path.begin(), path.end(), addr.sun_path);
  return true;
}
inline void Reply(int fd, const std::string& error = {})
{
  if (fd < 0) return;
  const std::string text = error.empty() ? "ok\n" : "error\t" + error + "\n";
  (void)!send(fd, text.data(), text.size(), MSG_NOSIGNAL);
  close(fd);
}
inline std::string Send(const std::string& dir, std::string surface, const std::string& pid, const HarborRoute::Target& target, const std::string& action = {}, const std::string& token = {})
{
  surface = Surface(surface);
  if (pid.empty() || pid.size() > 12 || !std::all_of(pid.begin(), pid.end(), [](char c) { return c >= '0' && c <= '9'; }) || std::stoll(pid) <= 0)
    throw std::invalid_argument("--target-pid requires the exact current native Harbor UI PID.");
  if ((action.empty() && !target.Present()) || !Field(target.chat) || !Field(target.topic) || !Field(target.profile) ||
      (!action.empty() && action != "inspect" && action != "park") || !Field(token))
    throw std::invalid_argument("Targeted Harbor control requires a valid conversation route.");
  sockaddr_un addr;
  if (!Address(Path(dir, surface), addr)) throw std::invalid_argument("Harbor control socket path is too long for this configuration directory.");
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) throw std::runtime_error("Could not create Harbor control connection.");
  NoSigPipe(fd);
  if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || !SameUser(fd))
  { close(fd); throw std::runtime_error("The selected Harbor UI is unavailable. Refresh Desk before trying again; no window was opened."); }
  const auto request = pid + '\t' + surface + '\t' + (action.empty() ? target.chat : "__" + action + "__") + '\t' + (action.empty() ? target.topic : token) + '\t' + (action.empty() ? target.profile : "") + '\n';
  if (send(fd, request.data(), request.size(), MSG_NOSIGNAL) != ssize_t(request.size()))
  { close(fd); throw std::runtime_error("Harbor control delivery is unknown. Inspect the selected window before retrying."); }
  std::string response;
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(action == "inspect" ? 2 : 45);
  while (response.find('\n') == std::string::npos && response.size() <= 1024)
  {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(until - std::chrono::steady_clock::now()).count();
    if (left <= 0) break;
    pollfd ready = { fd, POLLIN, 0 };
    int rv = poll(&ready, 1, int(left));
    if (rv < 0 && errno == EINTR) continue;
    if (rv <= 0) break;
    char bytes[256]; ssize_t count = recv(fd, bytes, sizeof(bytes), 0);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) break;
    response.append(bytes, size_t(count));
  }
  close(fd);
  if (response == "ok\n") return response;
  if (action == "inspect" && response.rfind("pcl.harbor.ui.v1\t", 0) == 0 && response.back() == '\n') return response;
  if (response.rfind("error\t", 0) == 0 && response.back() == '\n' && Field(response.substr(6, response.size() - 7)))
    throw std::runtime_error(response.substr(6, response.size() - 7));
  throw std::runtime_error("Harbor control outcome is unknown. Inspect the selected window before retrying.");
}
class Server
{
  struct Client { int fd; std::string bytes; std::chrono::steady_clock::time_point since; };
  int m_Listen = -1, m_Lock = -1;
  std::string m_Path, m_Surface;
  std::vector<Client> m_Clients;
public:
  ~Server() { Stop(); }
  void Stop()
  {
    for (auto& c : m_Clients) close(c.fd);
    m_Clients.clear();
    if (m_Listen >= 0) { close(m_Listen); m_Listen = -1; }
    if (m_Lock >= 0) { unlink(m_Path.c_str()); close(m_Lock); m_Lock = -1; }
  }
  bool Start(const std::string& dir, const std::string& surface)
  {
    try { m_Surface = Surface(surface); } catch (...) { return false; }
    m_Path = Path(dir, m_Surface);
    sockaddr_un addr;
    if (!Address(m_Path, addr)) return false;
    m_Lock = open((m_Path + ".lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (m_Lock < 0) return false;
    if (flock(m_Lock, LOCK_EX | LOCK_NB) != 0) { close(m_Lock); m_Lock = -1; return false; }
    struct stat existing;
    if (lstat(m_Path.c_str(), &existing) == 0)
    {
      if (!S_ISSOCK(existing.st_mode) || existing.st_uid != geteuid())
      { close(m_Lock); m_Lock = -1; return false; }
      unlink(m_Path.c_str()); // only under this surface's lifetime registration lock
    }
    m_Listen = socket(AF_UNIX, SOCK_STREAM, 0);
    if (m_Listen < 0) { Stop(); return false; }
    fcntl(m_Listen, F_SETFD, FD_CLOEXEC); fcntl(m_Listen, F_SETFL, O_NONBLOCK);
    const auto mask = umask(0177);
    const int bound = bind(m_Listen, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    umask(mask);
    if (bound != 0 || listen(m_Listen, 8) != 0) { Stop(); return false; }
    return true;
  }
  void Poll(const std::function<void(const HarborRoute::Target&, int)>& apply,
    const std::function<void(const std::string&, const std::string&, int)>& control = {})
  {
    if (m_Listen < 0) return;
    int fd;
    while ((fd = accept(m_Listen, nullptr, nullptr)) >= 0)
    {
      if (!SameUser(fd) || m_Clients.size() >= 8) { close(fd); continue; }
      NoSigPipe(fd); fcntl(fd, F_SETFD, FD_CLOEXEC); fcntl(fd, F_SETFL, O_NONBLOCK);
      m_Clients.push_back({ fd, {}, std::chrono::steady_clock::now() });
    }
    for (auto it = m_Clients.begin(); it != m_Clients.end();)
    {
      char bytes[512]; const auto n = recv(it->fd, bytes, sizeof(bytes), 0);
      if (n > 0) it->bytes.append(bytes, size_t(n));
      bool remove = n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR);
      if (it->bytes.size() > 1024 || std::chrono::steady_clock::now() - it->since > std::chrono::seconds(5)) remove = true;
      if (it->bytes.find('\n') != std::string::npos)
      {
        try
        {
          auto line = it->bytes;
          if (line.back() != '\n' || line.find('\n') != line.size() - 1) throw std::invalid_argument("Malformed Harbor UI request.");
          line.pop_back();
          std::vector<std::string> fields; size_t start = 0;
          for (size_t at = 0; at <= line.size(); ++at)
            if (at == line.size() || line[at] == '\t') { fields.push_back(line.substr(start, at - start)); start = at + 1; }
          if (fields.size() != 5 || !std::all_of(fields.begin(), fields.end(), Field)) throw std::invalid_argument("Malformed Harbor UI request.");
          if (fields[0] != std::to_string(getpid()) || fields[1] != m_Surface)
            throw std::invalid_argument("The selected Harbor UI changed. Refresh Desk before trying again; no conversation changed.");
          if (control && fields[4].empty() && (fields[2] == "__inspect__" || fields[2] == "__park__"))
          { control(fields[2], fields[3], it->fd); it->fd = -1; remove = true; }
          else {
          HarborRoute::Target target{ fields[2], fields[3], fields[4], {} }; HarborRoute::Validate(target);
          if (!target.Present()) throw std::invalid_argument("Harbor UI request requires a conversation.");
          apply(target, it->fd); it->fd = -1; }
        }
        catch (const std::exception& error) { Reply(it->fd, error.what()); it->fd = -1; }
        remove = true;
      }
      if (remove) { if (it->fd >= 0) close(it->fd); it = m_Clients.erase(it); }
      else ++it;
    }
  }
};
}
