#include "session.h"
#include "sessiongeneration.h"
#include "update.h"
#include "wire.h"
#include "traceprotocol.h"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <poll.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <termios.h>
#include <streambuf>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0 // macOS: SO_NOSIGPIPE is set on each socket instead
#endif

namespace HarborSession
{
namespace
{
// A window that stops reading (for example a stopped terminal) must not exhaust the backend or
// stall other windows. Budget: 128 MiB of queued encoded bytes (including frame
// headers) per peer; kernel socket buffers are separate. Beyond that only this
// window is closed, with a clear retained-session-ended outcome.
const size_t QueueLimit = 2 * (size_t)HarborWire::MaxFrame;

using Deadline = std::chrono::steady_clock::time_point;

// One absolute deadline bounds both connection setup and the entire welcome, including a
// peer that keeps sending partial bytes. Ordinary windows retain their existing blocking I/O.
bool
WaitSocket(int fd, short events, const Deadline& deadline)
{
  while (true)
  {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
    if (left <= 0) { errno = ETIMEDOUT; return false; }
    pollfd p = { fd, events, 0 };
    const int rv = poll(&p, 1, int(left));
    if (rv < 0 && errno == EINTR) continue;
    if (rv == 0) errno = ETIMEDOUT;
    return rv > 0;
  }
}

// Native prompts keep using std::cin and tty fd 0 (including GetPass termios).
// Only their stream buffer changes: poll includes a cancellation pipe, so a lender's
// socket death ends a getline even when its PTY master is still open elsewhere.
// This lives for the backend process, including native auth threads during shutdown.
class TerminalInput : public std::streambuf
{
public:
  TerminalInput() { if (pipe(m_Cancel) != 0) std::abort();
    for (int fd : m_Cancel) { fcntl(fd, F_SETFD, FD_CLOEXEC); fcntl(fd, F_SETFL, O_NONBLOCK); } }
  void Bind(int fd)
  {
    std::lock_guard<std::mutex> lock(m_Mutex);
    char drain[32];
    while (read(m_Cancel[0], drain, sizeof(drain)) > 0) { }
    if (m_Input >= 0) close(m_Input);
    m_Input = dup(fd);
    m_Cancelled = false;
    m_ReadStarted = false;
  }
  bool ReadStarted()
  { std::lock_guard<std::mutex> lock(m_Mutex); return m_ReadStarted; }
  void Cancel()
  {
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Cancelled = true;
    if (m_Input >= 0) { close(m_Input); m_Input = -1; }
    char c = 1;
    (void)!write(m_Cancel[1], &c, 1);
  }
protected:
  int_type underflow() override
  {
    int input;
    {
      std::lock_guard<std::mutex> lock(m_Mutex);
      if (m_Cancelled || m_Input < 0) return traits_type::eof();
      m_ReadStarted = true;
      input = dup(m_Input);
    }
    if (input < 0) return traits_type::eof();
    pollfd fds[2] = { { input, POLLIN, 0 }, { m_Cancel[0], POLLIN, 0 } };
    while (true)
    {
      int rv = poll(fds, 2, -1);
      if (rv < 0 && errno == EINTR) continue;
      if (rv < 0 || fds[1].revents) break;
      if (fds[0].revents)
      {
        ssize_t n = read(input, &m_Byte, 1);
        if (n < 0 && errno == EINTR) continue;
        close(input);
        if (n != 1) return traits_type::eof();
        setg(&m_Byte, &m_Byte, &m_Byte + 1);
        return traits_type::to_int_type(m_Byte);
      }
    }
    close(input);
    return traits_type::eof();
  }
private:
  std::mutex m_Mutex;
  int m_Cancel[2];
  int m_Input = -1;
  bool m_Cancelled = true;
  bool m_ReadStarted = false;
  char m_Byte = 0;
};
TerminalInput& PromptInput() { static TerminalInput input; return input; }

bool
WriteAll(int fd, const std::string& data)
{
  size_t done = 0;
  while (done < data.size())
  {
    ssize_t n = send(fd, data.data() + done, data.size() - done, MSG_NOSIGNAL);
    if (n < 0 && errno == EINTR)
      continue;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
    {
      pollfd p = { fd, POLLOUT, 0 };
      poll(&p, 1, -1);
      continue;
    }
    if (n <= 0)
    { HarborTrace::Os("socket.write.failed", n < 0 ? errno : EPIPE); return false; }
    done += (size_t)n;
  }
  return true;
}

// Reads one complete frame (blocking). False on EOF or a malformed frame.
bool
ReadFrame(int fd, std::string& buf, HarborWire::Kind& kind, std::string& payload, const Deadline* deadline = nullptr)
{
  while (true)
  {
    if (deadline && std::chrono::steady_clock::now() >= *deadline)
    { errno = ETIMEDOUT; return false; }
    bool have = false;
    if (!HarborWire::NextFrame(buf, kind, payload, have))
    { HarborTrace::Event("socket.frame.rejected"); return false; }
    if (have)
      return true;
    if (deadline && !WaitSocket(fd, POLLIN, *deadline)) return false;
    char chunk[65536];
    ssize_t n = recv(fd, chunk, sizeof(chunk), deadline ? MSG_DONTWAIT : 0);
    if (n < 0 && errno == EINTR)
      continue;
    if (deadline && n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
    if (n <= 0)
    { HarborTrace::Os("socket.read.ended", n < 0 ? errno : 0, {{"cause", n == 0 ? "eof" : "read_error"}}); return false; }
    buf.append(chunk, (size_t)n);
  }
}

bool
SameUser(int fd)
{
#if defined(__APPLE__) || defined(__FreeBSD__)
  uid_t uid = 0;
  gid_t gid = 0;
  return getpeereid(fd, &uid, &gid) == 0 && uid == geteuid();
#else
  struct ucred cred;
  socklen_t len = sizeof(cred);
  return getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) == 0 && cred.uid == geteuid();
#endif
}

void
NoSigPipe(int fd)
{
#ifdef SO_NOSIGPIPE
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#else
  (void)fd;
#endif
}

std::string
WelcomePayload(uint32_t connId, const std::vector<Profile>& profiles)
{
  HarborWire::Writer w;
  w.S(HarborUpdate::Version());
  w.U32(connId);
  w.U32((uint32_t)profiles.size());
  for (auto& p : profiles)
  {
    w.S(p.id);
    w.S(p.displayName);
  }
  return w.buf;
}
}

std::string
SocketPath(const std::string& p_AppDir)
{
  return p_AppDir + "/session.sock";
}

std::string
SocketPathProblem(const std::string& p_Path)
{
  sockaddr_un addr;
  if (p_Path.size() < sizeof(addr.sun_path))
    return std::string();
  return "Harbor's session socket path is " + std::to_string(p_Path.size()) + " bytes, over this system's " +
         std::to_string(sizeof(addr.sun_path) - 1) + "-byte limit:\n  " + p_Path +
         "\nChoose a shorter configuration directory with -d.";
}

bool
CompatibleVersions(const std::string& p_Client, const std::string& p_Backend)
{
  // Before these upgrade exceptions, session.cpp, session.h, wire.h and protocol.h were
  // byte-identical in released 0.1.22 (c6e8ea23), strip 0.1.23 (6618ded3) and composer
  // 0.1.24 (c23d36ef). The pinned 0.1.24 (3cc477f6) session/wire/protocol sources
  // also match 0.1.25 before this allowlist addition. The pinned 0.1.25 (885ebd85)
  // sources also match pre-allowlist 0.1.26 (63453e4b). Pinned 0.1.26 (750136da)
  // sources match pre-allowlist 0.1.27 (dd0541b2). Released 0.1.27 (c9935c28)
  // matches pre-allowlist 0.1.28 (09db5368). No general semver range.
  return p_Client == p_Backend ||
         (p_Client == "0.1.23" && p_Backend == "0.1.22") ||
         (p_Client == "0.1.24" && (p_Backend == "0.1.22" || p_Backend == "0.1.23")) ||
         (p_Client == "0.1.25" && p_Backend == "0.1.24") ||
         (p_Client == "0.1.26" && p_Backend == "0.1.25") ||
         (p_Client == "0.1.27" && p_Backend == "0.1.26") ||
         (p_Client == "0.1.28" && p_Backend == "0.1.27");
}

// Window side ---------------------------------------------------------------------------------

namespace
{
int
ConnectSocket(const std::string& path, const Deadline* deadline = nullptr)
{
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0)
  { HarborTrace::Os("socket.create.failed", errno); return -1; }
  sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
  if (deadline && fcntl(fd, F_SETFL, O_NONBLOCK) != 0)
  { const int error = errno; close(fd); errno = error; return -1; }
  int connected = connect(fd, (sockaddr*)&addr, sizeof(addr));
  if (deadline && connected != 0 && errno == EINPROGRESS)
  {
    if (WaitSocket(fd, POLLOUT, *deadline))
    {
      int error = 0;
      socklen_t size = sizeof(error);
      if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) == 0)
      { connected = error == 0 ? 0 : -1; if (error) errno = error; }
    }
  }
  if (connected != 0 || !SameUser(fd))
  {
    const int error = connected != 0 ? errno : EACCES;
    HarborTrace::Os(connected != 0 ? "socket.connect.failed" : "socket.peer.rejected", error);
    close(fd);
    errno = error;
    return -1;
  }
  HarborTrace::Event("socket.connected");
  fcntl(fd, F_SETFD, FD_CLOEXEC);
  NoSigPipe(fd);
  return fd;
}

bool
ReadWelcome(int fd, std::string& buf, Attachment& out, const Deadline* deadline = nullptr)
{
  HarborWire::Kind kind;
  std::string payload;
  errno = 0;
  if (!ReadFrame(fd, buf, kind, payload, deadline))
  {
    HarborTrace::Event("handshake.failed", {{"cause", "frame_or_eof"}});
    out.error = deadline && errno == ETIMEDOUT ? "Harbor's session did not send a welcome within two seconds. Try again; if it persists, close the other Harbor windows and run harbor again." :
                                               "Harbor's session ended before it started.";
    return false;
  }
  if (kind == HarborWire::KindEnded)
  {
    HarborTrace::Event("handshake.failed", {{"cause", "backend_startup_failure"}});
    out.error = payload.empty() ? "Harbor's session ended before it started." : payload;
    return false;
  }
  HarborWire::Reader r(payload);
  if (kind != HarborWire::KindWelcome)
  {
    HarborTrace::Event("handshake.failed", {{"cause", "unexpected_frame"}});
    out.error = "Harbor's session sent an unexpected first frame.";
    return false;
  }
  const std::string version = r.S();
  if (r.ok && !CompatibleVersions(HarborUpdate::Version(), version))
  {
    HarborTrace::Event("handshake.failed", {{"cause", "version_mismatch"}, {"expected_version", HarborUpdate::Version()}, {"peer_version", version.size() <= 24 && version.find_first_not_of("0123456789.") == std::string::npos ? version : "invalid"}});
    out.error = "Harbor " + version + " is still running in other windows; this is Harbor " +
                HarborUpdate::Version() + ". Close the other Harbor windows, then run harbor again.";
    return false;
  }
  out.connectionId = r.U32(); // ownership identity, no protocol change
  for (uint32_t i = 0, n = r.Count(); r.ok && i < n; ++i)
  {
    Profile p;
    p.index = int(i);
    p.id = r.S();
    p.displayName = r.S();
    out.profiles.push_back(p);
  }
  if (!r.Done())
  {
    HarborTrace::Event("handshake.failed", {{"cause", "malformed_welcome"}});
    out.error = "Harbor's session sent a malformed welcome.";
    return false;
  }
  HarborTrace::Event("handshake.ready", {{"peer_version", version}, {"connection_id", HarborTrace::Number(out.connectionId)}, {"profiles", HarborTrace::Number(out.profiles.size())}});
  return true;
}
}

std::string
CheckSession(const std::string& p_AppDir)
{
  const std::string path = SocketPath(p_AppDir);
  const std::string problem = SocketPathProblem(path);
  if (!problem.empty()) return problem;

  // Only an absent path or a stale, same-user socket is a safe no-session result. Do not
  // follow symlinks, certify another user's endpoint or hide filesystem/access failures.
  struct stat st = {};
  if (lstat(path.c_str(), &st) != 0)
  {
    if (errno == ENOENT) return {};
    return "Could not inspect Harbor's session socket " + path + ": " + strerror(errno);
  }
  if (!S_ISSOCK(st.st_mode) || st.st_uid != geteuid())
    return "Harbor's session path is not a same-user socket: " + path + ". Choose a different configuration directory with -d.";

  const Deadline deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  const int fd = ConnectSocket(path, &deadline);
  if (fd < 0)
  {
    const int error = errno;
    if (error == ENOENT) return {};
    if (error == ECONNREFUSED)
    {
      struct stat now = {};
      if (lstat(path.c_str(), &now) == 0 && S_ISSOCK(now.st_mode) && now.st_uid == geteuid() &&
          now.st_dev == st.st_dev && now.st_ino == st.st_ino)
        return {}; // leave the same stale socket untouched; Attach owns locked recovery
    }
    return "Could not connect to Harbor's session socket " + path + ": " + strerror(error);
  }
  std::string buf;
  Attachment out;
  const bool ok = ReadWelcome(fd, buf, out, &deadline);
  close(fd);
  return ok ? std::string() : out.error;
}

Attachment
Attach(const std::string& p_AppDir, const std::string& p_Self, const std::vector<std::string>& p_BackendArgs)
{
  Attachment out;
  HarborTrace::Event("session.attach.attempt", {{"config_id", HarborTrace::Token(p_AppDir)}});
  const std::string path = SocketPath(p_AppDir);
  const std::string problem = SocketPathProblem(path);
  if (!problem.empty())
  {
    HarborTrace::Event("session.attach.failed", {{"cause", "socket_path_limit"}, {"bytes", HarborTrace::Number(path.size())}});
    out.error = problem;
    return out;
  }

  // One ordinary advisory lock serialises connect-or-spawn: a second window started at the same
  // moment waits here until the first window's backend is listening, then connects to it.
  const std::string lockPath = p_AppDir + "/session.lock";
  HarborTrace::Event("session.lock.attempt");
  int lockFd = open(lockPath.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  if (lockFd < 0 || flock(lockFd, LOCK_EX) != 0)
  {
    HarborTrace::Os("session.lock.failed", errno, {{"operation", lockFd < 0 ? "open" : "flock"}});
    out.error = "Could not lock " + lockPath + ": " + strerror(errno);
    if (lockFd >= 0)
      close(lockFd);
    return out;
  }

  HarborTrace::Event("session.lock.acquired");
  std::string buf;
  int fd = ConnectSocket(path);
  if (fd < 0)
  {
    HarborTrace::Event("backend.spawn.attempt");
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0)
    {
      HarborTrace::Os("backend.socketpair.failed", errno);
      out.error = std::string("Could not create session connection: ") + strerror(errno);
      close(lockFd);
      return out;
    }
    pid_t pid = fork();
    if (pid == 0)
    {
      close(pair[0]);
      close(lockFd);
      setsid();
      int null = open("/dev/null", O_RDWR);
      if (null >= 0)
      {
        dup2(null, 0);
        dup2(null, 1);
        dup2(null, 2);
        if (null > 2)
          close(null);
      }
      std::vector<std::string> args = { p_Self, BackendArg, std::to_string(pair[1]) };
      args.insert(args.end(), p_BackendArgs.begin(), p_BackendArgs.end());
      std::vector<char*> argv;
      for (auto& a : args)
        argv.push_back(const_cast<char*>(a.c_str()));
      argv.push_back(nullptr);
      execv(p_Self.c_str(), argv.data());
      HarborTrace::Os("backend.exec.failed", errno);
      _exit(127);
    }
    const int forkError = errno;
    close(pair[1]);
    if (pid < 0)
    {
      HarborTrace::Os("backend.fork.failed", forkError);
      out.error = std::string("Could not start Harbor's session: ") + strerror(forkError);
      close(pair[0]);
      close(lockFd);
      return out;
    }
    HarborTrace::Event("backend.spawned", {{"child_pid", HarborTrace::Number(pid)}});
    fd = pair[0];
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    NoSigPipe(fd);
  }

  // The backend listens before it sends a welcome, so releasing the lock after the welcome
  // guarantees the next window connects instead of spawning.
  const bool ok = ReadWelcome(fd, buf, out);
  if (ok)
  {
    struct stat socket{};
    if (lstat(path.c_str(), &socket) || !S_ISSOCK(socket.st_mode) || socket.st_uid != geteuid() ||
        !HarborSessionGeneration::Read(p_AppDir, socket, out.generation))
      out.error = "Could not identify Harbor's active session generation. Existing windows were retained.";
  }
  close(lockFd);
  if (!ok || !out.error.empty())
  {
    close(fd);
    return out;
  }
  out.fd = fd;
  out.pending = buf;
  return out;
}

int
AcquireSlot(const std::string& p_AppDir)
{
  for (int slot = 0; slot < 1024; ++slot)
  {
    const std::string path = p_AppDir + "/harbor-state.slot" + std::to_string(slot) + ".lock";
    int fd = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0)
    { HarborTrace::Os("window.slot.failed", errno); return -1; }
    if (flock(fd, LOCK_EX | LOCK_NB) == 0)
      return slot; // fd intentionally held until exit
    close(fd);
  }
  return -1;
}

Connection::Connection(int p_Fd, const std::string& p_Pending, uint32_t p_Id)
  : m_Id(p_Id)
  , m_Fd(p_Fd)
  , m_Pending(p_Pending)
{
}
Connection::~Connection()
{
  Stop();
}
void
Connection::SetHandler(const std::string& p_ProfileId, const std::function<void(std::shared_ptr<ServiceMessage>)>& p_Handler)
{
  std::lock_guard<std::mutex> lock(m_Mutex);
  m_Handlers[p_ProfileId] = p_Handler;
}
void
Connection::Start()
{
  if (!m_Reader.joinable())
    m_Reader = std::thread([this] { ReadLoop(); });
}
void
Connection::Stop()
{
  m_Stopping = true;
  if (m_Fd >= 0)
    shutdown(m_Fd, SHUT_RDWR);
  if (m_Reader.joinable())
    m_Reader.join();
  {
    std::lock_guard<std::mutex> lock(m_WriteMutex);
    if (m_Fd >= 0)
      close(m_Fd);
    m_Fd = -1;
  }
  // No reader or writer can now confirm another send. Deliver the originals
  // synchronously to the UI queue while its final state save is still possible.
  RecoverUnknown();
}
bool
Connection::Send(const std::string& p_ProfileId, std::shared_ptr<RequestMessage> p_Request, int p_ProfileIndex)
{
  std::string encoded;
  if (m_Ended || m_Stopping) { HarborTrace::Request("request.rejected_closed", p_Request, m_Id); return false; }
  if (!HarborWire::EncodeRequest(p_Request, encoded))
  {
    HarborTrace::Request("request.rejected_encoding", p_Request, m_Id);
    if (p_Request->GetMessageType() == SendMessageRequestType)
    {
      auto q = std::static_pointer_cast<SendMessageRequest>(p_Request);
      auto failed = std::make_shared<SendMessageNotify>(p_ProfileId);
      failed->chatId = q->chatId; failed->chatMessage = q->chatMessage;
      Dispatch(failed);
    }
    return false;
  }
  std::lock_guard<std::mutex> lock(m_WriteMutex);
  if (m_Ended || m_Stopping) return false;
  // The window's own sequence; the backend replaces the high half with this connection's identity.
  if (m_Seq == UINT32_MAX)
  {
    End("Harbor's window has used its request identities. Your drafts were saved; run harbor again.");
    return false;
  }
  p_Request->requestId = ++m_Seq;
  HarborTrace::Request("request.send.attempt", p_Request, m_Id, p_ProfileIndex);
  // Edits are not tracked: a successful native edit sends no reply, so its outcome is never
  // unknown in a way the window could report.
  if (p_Request->GetMessageType() == SendMessageRequestType)
  {
    auto unknown = std::make_shared<SendMessageNotify>(p_ProfileId);
    auto q = std::static_pointer_cast<SendMessageRequest>(p_Request);
    unknown->requestId = p_Request->requestId;
    unknown->chatId = q->chatId;
    unknown->chatMessage = q->chatMessage;
    std::lock_guard<std::mutex> state(m_Mutex);
    m_Unknown[p_Request->requestId] = unknown;
  }
  encoded.replace(4, 8, std::string(8, '\0'));
  for (int i = 0; i < 8; ++i)
    encoded[4 + i] = (char)(uint8_t)(p_Request->requestId >> (8 * i));
  HarborWire::Writer w;
  w.S(p_ProfileId);
  w.S(encoded);
  const auto frame = w.ok ? HarborWire::Frame(HarborWire::KindRequest, w.buf) : std::string();
  if (!frame.empty() && WriteAll(m_Fd, frame))
  { HarborTrace::Request("request.sent", p_Request, m_Id, p_ProfileIndex); return true; }
  HarborTrace::Request("request.send.failed", p_Request, m_Id);
  if (!m_Stopping)
    End("Harbor's session ended. Your drafts and unknown sends were saved; run harbor again.");
  return false;
}
void
Connection::Lent()
{
  HarborTrace::Event("terminal.lend.attempt", {{"connection_id", HarborTrace::Number(m_Id)}});
  // Pass this window's terminal (fds 0 and 1) to the backend for the native prompt.
  std::string frame = HarborWire::Frame(HarborWire::KindLent, std::string());
  struct iovec iov = { (void*)frame.data(), frame.size() };
  int fds[2] = { 0, 1 };
  char control[CMSG_SPACE(sizeof(fds))];
  memset(control, 0, sizeof(control));
  struct msghdr msg;
  memset(&msg, 0, sizeof(msg));
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = control;
  msg.msg_controllen = sizeof(control);
  struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
  cmsg->cmsg_level = SOL_SOCKET;
  cmsg->cmsg_type = SCM_RIGHTS;
  cmsg->cmsg_len = CMSG_LEN(sizeof(fds));
  memcpy(CMSG_DATA(cmsg), fds, sizeof(fds));
  std::lock_guard<std::mutex> lock(m_WriteMutex);
  if (m_Stopping) return;
  ssize_t sent;
  do { sent = sendmsg(m_Fd, &msg, MSG_NOSIGNAL); } while (sent < 0 && errno == EINTR);
  if (sent != (ssize_t)frame.size())
    End("Harbor could not lend this terminal. Your drafts were saved; run harbor again.");
}
std::string
Connection::EndReason()
{
  std::lock_guard<std::mutex> lock(m_Mutex);
  return m_EndReason;
}
void
Connection::Dispatch(std::shared_ptr<ServiceMessage> n)
{
  std::function<void(std::shared_ptr<ServiceMessage>)> handler;
  {
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (n->requestId != 0 && n->GetMessageType() == SendMessageNotifyType &&
        !std::static_pointer_cast<SendMessageNotify>(n)->pending)
      m_Unknown.erase(n->requestId);
    auto it = m_Handlers.find(n->profileId);
    if (it != m_Handlers.end())
      handler = it->second;
  }
  if (handler)
    handler(n);
}
void
Connection::End(const std::string& p_Reason)
{
  if (m_Ended.exchange(true))
    return;
  HarborTrace::Event("session.disconnected", {{"connection_id", HarborTrace::Number(m_Id)}});
  std::vector<std::string> profiles;
  {
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_EndReason = p_Reason;
    for (auto& h : m_Handlers)
      profiles.push_back(h.first);
  }
  RecoverUnknown();
  for (auto& p : profiles)
    Dispatch(std::make_shared<RequestAppExitNotify>(p));
}
void
Connection::RecoverUnknown()
{
  std::map<uint64_t, std::shared_ptr<SendMessageNotify>> unknown;
  {
    std::lock_guard<std::mutex> lock(m_Mutex);
    unknown.swap(m_Unknown);
  }
  // A local acceptance is not confirmation. Preserve complete originals, never replay.
  for (auto& u : unknown)
  {
    HarborTrace::Event("request.outcome_unknown", {{"connection_id", HarborTrace::Number(m_Id)}, {"request_id", HarborTrace::Number(u.first)}});
    Dispatch(u.second);
  }
}
void
Connection::ReadLoop()
{
  std::string buf = m_Pending;
  std::string lendProfile;
  while (!m_Stopping)
  {
    HarborWire::Kind kind;
    std::string payload;
    if (!ReadFrame(m_Fd, buf, kind, payload))
    {
      if (!m_Stopping)
        End("Harbor's session ended. Your drafts were saved; run harbor again.");
      return;
    }
    switch (kind)
    {
      case HarborWire::KindEvent:
      {
        auto n = HarborWire::DecodeNotify(payload);
        if (!n)
        {
          End("Harbor received a malformed session event and closed this window.");
          return;
        }
        HarborTrace::Result("request.result.received", n, m_Id);
        Dispatch(n);
        break;
      }
      case HarborWire::KindLend:
      {
        // Reuse Harbor's existing auth suspend branch; it calls Lent() once ncurses is left.
        lendProfile = payload;
        auto n = std::make_shared<ProtocolUiControlNotify>(payload);
        n->isTakeControl = true;
        Dispatch(n);
        break;
      }
      case HarborWire::KindReturn:
      {
        auto n = std::make_shared<ProtocolUiControlNotify>(payload.empty() ? lendProfile : payload);
        n->isTakeControl = false;
        Dispatch(n);
        break;
      }
      case HarborWire::KindEnded:
        End(payload);
        return;
      default:
        End("Harbor received an unknown session frame and closed this window.");
        return;
    }
  }
}

Remote::Remote(std::shared_ptr<Connection> p_Connection, const Profile& p_Profile)
  : m_Connection(p_Connection)
  , m_Profile(p_Profile)
{
}
bool
Remote::Login()
{
  m_Connection->Start();
  return true;
}
void
Remote::SendRequest(std::shared_ptr<RequestMessage> p_Request)
{
  m_Connection->Send(m_Profile.id, p_Request, m_Profile.index);
}
void
Remote::SetMessageHandler(const std::function<void(std::shared_ptr<ServiceMessage>)>& p_Handler)
{
  m_Connection->SetHandler(m_Profile.id, p_Handler);
}

// Backend side --------------------------------------------------------------------------------

namespace
{
struct Peer
{
  int fd = -1;
  uint32_t id = 0;
  std::string in;
  std::string out;
  bool closing = false;
  std::string endReason;
  std::vector<int> receivedFds;
  bool terminalReady = false;
};

class Backend : public std::enable_shared_from_this<Backend>
{
public:
  Backend(const std::string& appDir, const std::vector<std::shared_ptr<Protocol>>& protocols)
    : m_AppDir(appDir)
    , m_Protocols(protocols)
  {
    for (auto& p : protocols)
      m_Profiles.push_back(Profile{ p->GetProfileId(), p->GetProfileDisplayName() });
  }

  ~Backend() { for (int fd : m_Wake) if (fd >= 0) close(fd); }
  int Run(int inheritedFd, int* shutdownLock, const std::function<bool()>& shouldStop);
  void Handle(std::shared_ptr<ServiceMessage> n);

private:
  bool Listen(std::string& error);
  void AddPeer(int fd);
  void ClosePeer(const std::shared_ptr<Peer>& peer);
  void ReadPeer(const std::shared_ptr<Peer>& peer, std::vector<std::pair<std::string, std::shared_ptr<RequestMessage>>>& forward);
  void Enqueue(const std::shared_ptr<Peer>& peer, const std::string& frame);
  void Wake();
  void UiControl(std::shared_ptr<ProtocolUiControlNotify> n);
  void ReleaseTerminal();

  std::string m_AppDir;
  std::vector<std::shared_ptr<Protocol>> m_Protocols;
  std::vector<Profile> m_Profiles;
  std::mutex m_Mutex;
  std::condition_variable m_Cond;
  std::vector<std::shared_ptr<Peer>> m_Peers;
  std::map<std::string, std::string> m_ConnectFrames; // profile -> last ConnectNotify event frame
  int m_Listen = -1;
  HarborSessionGeneration::Publisher m_Generation;
  int m_Wake[2] = { -1, -1 };
  uint32_t m_NextId = 1;
  bool m_Exhausted = false;
  // terminal lending
  std::shared_ptr<Peer> m_Lender;
  std::string m_LendProfile;
  bool m_Borrowed = false;
  struct termios m_TerminalMode = {};
  bool m_HaveTerminalMode = false;
  bool m_ShuttingDown = false;
};

bool
Backend::Listen(std::string& error)
{
  const std::string path = SocketPath(m_AppDir);
  error = SocketPathProblem(path);
  if (!error.empty())
    return false;
  // The directory lock is held, so no other backend serves this directory: a leftover socket
  // is stale.
  unlink(path.c_str());
  m_Listen = socket(AF_UNIX, SOCK_STREAM, 0);
  sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
  const mode_t old = umask(0177);
  const bool bound = m_Listen >= 0 && bind(m_Listen, (sockaddr*)&addr, sizeof(addr)) == 0;
  const int bindError = errno;
  umask(old);
  if (!bound) errno = bindError;
  if (!bound || listen(m_Listen, 16) != 0)
  {
    HarborTrace::Os("backend.listen.failed", errno, {{"operation", bound ? "listen" : m_Listen >= 0 ? "bind" : "socket"}});
    error = "Could not listen on " + path + ": " + strerror(errno);
    return false;
  }
  chmod(path.c_str(), 0600);
  if (!m_Generation.Publish(m_AppDir, path))
  {
    error = "Could not publish Harbor's private session generation.";
    close(m_Listen);m_Listen = -1;unlink(path.c_str());
    return false;
  }
  fcntl(m_Listen, F_SETFD, FD_CLOEXEC);
  fcntl(m_Listen, F_SETFL, O_NONBLOCK);
  return true;
}

void
Backend::Wake()
{
  char c = 1;
  (void)!write(m_Wake[1], &c, 1);
}

void
Backend::Enqueue(const std::shared_ptr<Peer>& peer, const std::string& frame)
{
  if (peer->closing)
    return;
  if (peer->out.size() + frame.size() > QueueLimit)
  {
    HarborTrace::Event("backend.peer.queue_limit", {{"connection_id", HarborTrace::Number(peer->id)}});
    peer->closing = true;
    peer->endReason = "This Harbor window stopped reading, so its session ended. Run harbor again.";
    return;
  }
  peer->out += frame;
}

// Called by every protocol and message cache thread. Never blocks on a window.
void
Backend::Handle(std::shared_ptr<ServiceMessage> n)
{
  const auto profile = std::find_if(m_Profiles.begin(), m_Profiles.end(), [&](const Profile& p) { return p.id == n->profileId; });
  HarborTrace::Result("request.result.produced", n, uint32_t(n->requestId >> 32),
    profile == m_Profiles.end() ? -1 : int(std::distance(m_Profiles.begin(), profile)));
  if (n->GetMessageType() == ProtocolUiControlNotifyType)
  {
    UiControl(std::static_pointer_cast<ProtocolUiControlNotify>(n));
    return;
  }
  if (n->GetMessageType() == SendMessageNotifyType && n->requestId == 0)
    return; // a pending send from before this backend has no window owner
  std::string payload;
  if (!HarborWire::EncodeNotify(n, payload))
  {
    // Unsupported native events are not consumed by Harbor. A supported event that
    // exceeds the codec's memory budget ends the session instead of silently dropping it.
    switch (n->GetMessageType())
    {
      case ConnectNotifyType: case NewContactsNotifyType: case NewChatsNotifyType:
      case NewMessagesNotifyType: case SendMessageNotifyType: case DeleteMessageNotifyType:
      case ReceiveTypingNotifyType: case NewMessageStatusNotifyType: case NewMessageFileNotifyType:
      case RequestAppExitNotifyType: case NewMessageReactionsNotifyType: case AvailableReactionsNotifyType:
      case SearchMessagesNotifyType:
      {
        std::lock_guard<std::mutex> lock(m_Mutex);
        for (auto& peer : m_Peers)
        { peer->closing = true; peer->endReason = "Harbor's session received an event above its 64 MiB frame limit. Your drafts were saved; run harbor again."; }
        Wake();
        break;
      }
      default: break;
    }
    return;
  }
  const uint32_t target = (uint32_t)(n->requestId >> 32);
  if (target != 0)
  {
    // Restore the window's own sequence in the low half.
    const uint64_t low = n->requestId & 0xffffffffULL;
    for (int i = 0; i < 8; ++i)
      payload[4 + i] = (char)(uint8_t)(low >> (8 * i));
  }
  const std::string frame = HarborWire::Frame(HarborWire::KindEvent, payload);
  {
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (n->GetMessageType() == ConnectNotifyType)
      m_ConnectFrames[n->profileId] = frame;
    for (auto& peer : m_Peers)
      if (target == 0 || peer->id == target)
        Enqueue(peer, frame);
  }
  Wake();
}

void
Backend::UiControl(std::shared_ptr<ProtocolUiControlNotify> n)
{
  HarborTrace::Event("backend.terminal.control", {{"take_control", n->isTakeControl ? "yes" : "no"}});
  std::unique_lock<std::mutex> lock(m_Mutex);
  n->cancellable = true;
  if (m_ShuttingDown && n->isTakeControl)
  {
    n->isTakeControl = false;
    n->cancelled = true;
    return;
  }
  if (n->isTakeControl)
  {
    if (m_Borrowed || m_Lender)
    {
      n->isTakeControl = false; // another profile is prompting; TgChat retries
      return;
    }
    std::shared_ptr<Peer> lender;
    for (auto& peer : m_Peers)
      if (!peer->closing)
      {
        lender = peer;
        break;
      }
    if (!lender)
    {
      n->isTakeControl = false;
      return;
    }
    m_Lender = lender;
    m_LendProfile = n->profileId;
    Enqueue(lender, HarborWire::Frame(HarborWire::KindLend, n->profileId));
    Wake();
    m_Cond.wait(lock, [&] { return m_Lender != lender || lender->terminalReady || lender->closing; });
    if (m_Lender != lender || lender->closing || !lender->terminalReady || lender->receivedFds.size() != 2)
    {
      m_Lender.reset();
      n->isTakeControl = false;
      return;
    }
    m_HaveTerminalMode = tcgetattr(lender->receivedFds[0], &m_TerminalMode) == 0;
    PromptInput().Bind(lender->receivedFds[0]);
    dup2(lender->receivedFds[0], 0);
    dup2(lender->receivedFds[1], 1);
    std::cin.clear(); // an earlier prompt may have ended at detached end of input
    for (int fd : lender->receivedFds)
      close(fd);
    lender->receivedFds.clear();
    lender->terminalReady = false;
    m_Borrowed = true;
  }
  else
  {
    if (!m_Borrowed || m_LendProfile != n->profileId)
      return;
    ReleaseTerminal();
    m_Cond.notify_all();
    if (m_Lender)
      Enqueue(m_Lender, HarborWire::Frame(HarborWire::KindReturn, n->profileId));
    m_Lender.reset();
    lock.unlock();
    Wake();
  }
}

// Restores detached stdio. Caller holds m_Mutex.
void
Backend::ReleaseTerminal()
{
  PromptInput().Cancel();
  if (m_HaveTerminalMode) tcsetattr(0, TCSANOW, &m_TerminalMode);
  m_HaveTerminalMode = false;
  int null = open("/dev/null", O_RDWR);
  if (null >= 0)
  {
    dup2(null, 0);
    dup2(null, 1);
    close(null);
  }
  m_Borrowed = false;
}

void
Backend::AddPeer(int fd)
{
  fcntl(fd, F_SETFD, FD_CLOEXEC);
  fcntl(fd, F_SETFL, O_NONBLOCK);
  NoSigPipe(fd);
  std::lock_guard<std::mutex> lock(m_Mutex);
  if (m_Exhausted)
  {
    // Connection identities are never reused in one backend lifetime.
    std::string frame = HarborWire::Frame(HarborWire::KindEnded,
                                          "Harbor's session has served its maximum number of windows; close all windows and start again.");
    (void)!send(fd, frame.data(), frame.size(), MSG_NOSIGNAL);
    close(fd);
    return;
  }
  auto peer = std::make_shared<Peer>();
  peer->fd = fd;
  peer->id = m_NextId;
  if (m_NextId == 0xffffffffu)
    m_Exhausted = true;
  else
    ++m_NextId;
  peer->out = HarborWire::Frame(HarborWire::KindWelcome, WelcomePayload(peer->id, m_Profiles));
  // Late attach: replay the connection state so the window requests chats and contacts.
  for (auto& f : m_ConnectFrames)
    peer->out += f.second;
  m_Peers.push_back(peer);
  HarborTrace::Event("backend.peer.attached", {{"connection_id", HarborTrace::Number(peer->id)}, {"windows", HarborTrace::Number(m_Peers.size())}});
}

void
Backend::ClosePeer(const std::shared_ptr<Peer>& peer)
{
  HarborTrace::Event("backend.peer.closed", {{"connection_id", HarborTrace::Number(peer->id)}, {"cause", peer->endReason.empty() ? "transport_closed" : "session_ended"}});
  if (!peer->endReason.empty())
  {
    std::string frame = HarborWire::Frame(HarborWire::KindEnded, peer->endReason);
    (void)!send(peer->fd, frame.data(), frame.size(), MSG_NOSIGNAL);
  }
  close(peer->fd);
  for (int fd : peer->receivedFds)
    close(fd);
  peer->receivedFds.clear();
  if (m_Lender == peer)
  {
    // The lender's terminal is gone: return the backend to detached stdio so native reads see
    // end of input rather than a terminal nobody owns.
    if (m_Borrowed)
    {
      // Cancel first, but keep fd 0/1 until the existing prompt has finished
      // its own terminal cleanup and returns control. GetPass restores termios
      // after getline; closing/rebinding its fd concurrently races that ioctl.
      PromptInput().Cancel();
      if (!PromptInput().ReadStarted()) ReleaseTerminal();
      for (auto& survivor : m_Peers)
        if (survivor != peer)
        {
          survivor->closing = true;
          survivor->endReason = "Harbor's sign-in terminal closed. This session ended; your drafts were saved. Run harbor again to sign in.";
        }
    }
    m_Lender.reset();
  }
  m_Peers.erase(std::remove(m_Peers.begin(), m_Peers.end(), peer), m_Peers.end());
  m_Cond.notify_all();
}

void
Backend::ReadPeer(const std::shared_ptr<Peer>& peer,
                  std::vector<std::pair<std::string, std::shared_ptr<RequestMessage>>>& forward)
{
  // Parse after each read, and yield after 256 KiB so a busy peer cannot monopolise
  // the shared loop. A partial frame retains at most MaxFrame + its four-byte header.
  for (int reads = 0; reads < 4; ++reads)
  {
    char chunk[65536];
    struct iovec iov = { chunk, sizeof(chunk) };
    char control[CMSG_SPACE(sizeof(int) * 4)];
    struct msghdr msg = {};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);
    ssize_t n = recvmsg(peer->fd, &msg, 0);
    if (n < 0 && errno == EINTR) { --reads; continue; }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
    if (n <= 0) { HarborTrace::Os("backend.peer.read.ended", n < 0 ? errno : 0, {{"connection_id", HarborTrace::Number(peer->id)}, {"cause", n == 0 ? "eof" : "read_error"}}); peer->closing = true; return; }
    bool badControl = (msg.msg_flags & (MSG_CTRUNC | MSG_TRUNC)) != 0;
    for (struct cmsghdr* c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c))
    {
      if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS || c->cmsg_len < CMSG_LEN(0))
      { badControl = true; continue; }
      const size_t count = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
      for (size_t i = 0; i < count; ++i)
      {
        int fd = -1;
        memcpy(&fd, CMSG_DATA(c) + i * sizeof(int), sizeof(int));
        if (m_Lender == peer && !m_Borrowed && peer->receivedFds.size() < 2)
        {
          fcntl(fd, F_SETFD, FD_CLOEXEC);
          peer->receivedFds.push_back(fd);
        }
        else { close(fd); badControl = true; }
      }
    }
    if (badControl) { peer->closing = true; return; }
    peer->in.append(chunk, (size_t)n);
    while (true)
    {
      HarborWire::Kind kind;
      std::string payload;
      bool have = false;
      if (!HarborWire::NextFrame(peer->in, kind, payload, have))
      { peer->closing = true; return; }
      if (!have) break;
      if (kind == HarborWire::KindLent)
      {
        struct stat input = {}, output = {};
        if (!payload.empty() || m_Lender != peer || m_Borrowed || peer->terminalReady ||
            peer->receivedFds.size() != 2 || !isatty(peer->receivedFds[0]) || !isatty(peer->receivedFds[1]) ||
            fstat(peer->receivedFds[0], &input) || fstat(peer->receivedFds[1], &output) ||
            input.st_rdev != output.st_rdev)
        { peer->closing = true; return; }
        peer->terminalReady = true;
        m_Cond.notify_all();
        continue;
      }
      if (kind != HarborWire::KindRequest) { peer->closing = true; return; }
      HarborWire::Reader r(payload);
      const std::string profile = r.S();
      const std::string encoded = r.S();
      auto request = r.Done() ? HarborWire::DecodeRequest(encoded) : nullptr;
      const bool knownProfile = std::any_of(m_Profiles.begin(), m_Profiles.end(),
        [&](const Profile& p) { return p.id == profile; });
      if (!request || !knownProfile || (uint32_t)request->requestId == 0)
      { peer->closing = true; return; }
      request->requestId = ((uint64_t)peer->id << 32) | (request->requestId & 0xffffffffULL);
      HarborTrace::Request("request.backend.received", request, peer->id, int(std::distance(m_Profiles.begin(), std::find_if(m_Profiles.begin(), m_Profiles.end(), [&](const Profile& p) { return p.id == profile; }))));
      forward.emplace_back(profile, request);
    }
  }
}

int
Backend::Run(int inheritedFd, int* shutdownLock, const std::function<bool()>& shouldStop)
{
  signal(SIGPIPE, SIG_IGN);
  std::cin.rdbuf(&PromptInput());
  if (pipe(m_Wake) != 0)
  {
    HarborTrace::Os("backend.wake_pipe.failed", errno);
    FailBackend(inheritedFd, "Harbor's session could not start.");
    return 1;
  }
  fcntl(m_Wake[0], F_SETFL, O_NONBLOCK);
  fcntl(m_Wake[1], F_SETFL, O_NONBLOCK);
  std::string error;
  if (!Listen(error))
  {
    FailBackend(inheritedFd, error);
    return 1;
  }
  HarborTrace::Event("backend.ready", {{"profiles", HarborTrace::Number(m_Profiles.size())}});
  AddPeer(inheritedFd);

  auto handler = [weak = std::weak_ptr<Backend>(shared_from_this())](std::shared_ptr<ServiceMessage> n)
  { if (auto backend = weak.lock()) backend->Handle(n); };
  std::thread loginThread([this, handler]
  {
    for (auto& p : m_Protocols)
    {
      p->SetMessageHandler(handler);
      HarborTrace::Event("profile.login.attempt");
      const bool loggedIn = p->Login();
      HarborTrace::Event("profile.login.result", {{"accepted", loggedIn ? "yes" : "no"}});
    }
  });

  while (true)
  {
    std::vector<pollfd> fds;
    std::vector<std::shared_ptr<Peer>> peers;
    std::vector<uint32_t> closed;
    bool done = false;
    {
      std::lock_guard<std::mutex> lock(m_Mutex);
      if (shouldStop && shouldStop())
        for (auto& peer : m_Peers)
        { peer->closing = true; peer->endReason = "Harbor's session stopped. Your drafts were saved; run harbor again."; }
      while (true)
      {
        auto it = std::find_if(m_Peers.begin(), m_Peers.end(), [](const auto& peer) { return peer->closing; });
        if (it == m_Peers.end()) break;
        auto peer = *it;
        closed.push_back(peer->id);
        ClosePeer(peer);
      }
      done = m_Peers.empty();
      for (auto& peer : m_Peers)
      {
        fds.push_back({ peer->fd, (short)(POLLIN | (peer->out.empty() ? 0 : POLLOUT)), 0 });
        peers.push_back(peer);
      }
    }
    // A closed view's current chat no longer needs a subscription (outside the lock: a
    // protocol may answer synchronously through Handle()).
    for (uint32_t id : closed)
      for (auto& p : m_Protocols)
      {
        auto clear = std::make_shared<SetCurrentChatRequest>();
        clear->requestId = (uint64_t)id << 32;
        p->SendRequest(clear);
      }
    int timeout = -1;
    if (done)
    {
      // Last window gone. Windows connect under the session lock, so holding it while draining
      // the listener closes the race with a window that is connecting right now (for example a
      // restarting window). Never block on it: that window may be waiting for our welcome.
      const std::string lockPath = m_AppDir + "/session.lock";
      int lockFd = open(lockPath.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
      if (lockFd >= 0 && flock(lockFd, LOCK_EX | LOCK_NB) == 0)
      {
        int fd;
        while ((fd = accept(m_Listen, nullptr, nullptr)) >= 0)
        {
          if (SameUser(fd))
            AddPeer(fd);
          else
            close(fd);
        }
        bool empty = false;
        {
          std::lock_guard<std::mutex> lock(m_Mutex);
          empty = m_Peers.empty();
        }
        if (empty)
        {
          close(m_Listen);
          m_Listen = -1;
          unlink(SocketPath(m_AppDir).c_str());
          // Keep connect-or-spawn fenced until main has released native profile/config
          // locks. Otherwise an immediate restart can spawn into an owner still cleaning up.
          if (shutdownLock) *shutdownLock = lockFd;
          else close(lockFd);
          break;
        }
        close(lockFd);
        continue;
      }
      if (lockFd >= 0)
        close(lockFd);
      timeout = 50; // a window holds the lock; its connection is about to arrive
    }
    fds.push_back({ m_Listen, POLLIN, 0 });
    fds.push_back({ m_Wake[0], POLLIN, 0 });
    if (poll(fds.data(), fds.size(), timeout) < 0 && errno != EINTR)
    { HarborTrace::Os("backend.poll.failed", errno); break; }

    if (fds[fds.size() - 1].revents)
    {
      char drain[256];
      while (read(m_Wake[0], drain, sizeof(drain)) > 0)
      {
      }
    }
    if (fds[fds.size() - 2].revents & POLLIN)
    {
      int fd;
      while ((fd = accept(m_Listen, nullptr, nullptr)) >= 0)
      {
        if (!SameUser(fd))
        {
          close(fd);
          continue;
        }
        AddPeer(fd);
      }
    }
    std::vector<std::pair<std::string, std::shared_ptr<RequestMessage>>> forward;
    {
      std::lock_guard<std::mutex> lock(m_Mutex);
      for (size_t i = 0; i < peers.size(); ++i)
      {
        auto& peer = peers[i];
        if (peer->closing)
          continue;
        if (fds[i].revents & (POLLIN | POLLHUP | POLLERR))
          ReadPeer(peer, forward);
        if (!peer->closing && !peer->out.empty())
        {
          ssize_t n = send(peer->fd, peer->out.data(), peer->out.size(), MSG_NOSIGNAL);
          if (n > 0)
            peer->out.erase(0, (size_t)n);
          else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
            peer->closing = true;
        }
      }
    }
    // Forward outside the lock: a protocol may answer synchronously through Handle().
    for (auto& f : forward)
      for (auto& p : m_Protocols)
        if (p->GetProfileId() == f.first)
          p->SendRequest(f.second);
  }

  HarborTrace::Event("backend.shutdown");
  // Stop listening before the caller releases the locks.
  if (m_Listen >= 0)
  {
    close(m_Listen);
    unlink(SocketPath(m_AppDir).c_str());
  }
  {
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_ShuttingDown = true;
    PromptInput().Cancel();
    if (m_Borrowed && !PromptInput().ReadStarted()) ReleaseTerminal();
    m_Lender.reset();
    m_Cond.notify_all();
  }
  if (loginThread.joinable()) loginThread.join();
  {
    std::unique_lock<std::mutex> lock(m_Mutex);
    m_Cond.wait(lock, [&] { return !m_Borrowed; });
  }
  return 0;
}
}

int
RunBackend(int p_InheritedFd, const std::string& p_AppDir, const std::vector<std::shared_ptr<Protocol>>& p_Protocols,
           const std::function<void(const std::function<void(std::shared_ptr<ServiceMessage>)>&)>& p_SetCacheHandler, int* p_ShutdownLock, const std::function<bool()>& p_ShouldStop)
{
  auto backend = std::make_shared<Backend>(p_AppDir, p_Protocols);
  p_SetCacheHandler([weak = std::weak_ptr<Backend>(backend)](std::shared_ptr<ServiceMessage> n)
  { if (auto session = weak.lock()) session->Handle(n); });
  const int rv = backend->Run(p_InheritedFd, p_ShutdownLock, p_ShouldStop);
  p_SetCacheHandler(nullptr);
  return rv;
}

void
FailBackend(int p_InheritedFd, const std::string& p_Message)
{
  // The caller logs the precise native cause. This path never copies arbitrary frame payloads.
  HarborTrace::Event("backend.startup.failed");
  if (p_InheritedFd < 0)
    return;
  WriteAll(p_InheritedFd, HarborWire::Frame(HarborWire::KindEnded, p_Message));
  close(p_InheritedFd);
}
}
