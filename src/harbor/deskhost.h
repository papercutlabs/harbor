#pragma once
#include "trace.h"
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <poll.h>
#include <spawn.h>
#include <string>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
#include <utility>

extern char** environ;

// One exact owned child, never a named service. It may render only after a matching explicit yield.
// A protocol failure retires/reaps the child before returning; it never retries an unknown action.
class HarborDeskHost
{
public:
  enum class Prepared { Ready, Rejected, Cancelled, Failed };
  ~HarborDeskHost() { Stop(); }
  static bool Eligible(const std::vector<std::string>& argv)
  {
    return getenv("HARBOR_DESK_WARM") && std::string(getenv("HARBOR_DESK_WARM")) == "1" &&
      argv.size() == 5 && !argv[0].empty() && argv[0][0] == '/' && !argv[1].empty() && argv[1][0] == '/' &&
      argv[2] == "desk" && argv[3] == "--view" &&
      (argv[4] == "everything" || argv[4] == "quota" || argv[4] == "now" || argv[4] == "help" || argv[4] == "focus");
  }
  std::string Error() const
  {
    std::string safe;
    for (unsigned char c : m_Error) { if (safe.size() == 160) break; safe += c >= 32 && c <= 126 ? char(c) : ' '; }
    const auto home = getenv("HOME"); if (home && *home) { const auto at = safe.find(home); if (at != std::string::npos) safe.replace(at, strlen(home), "~"); }
    return safe;
  }
  const std::string& Diagnostics() const { return m_Diagnostics; }
  std::string DiagnosticName() const { const auto at = m_Diagnostics.find_last_of('/'); return at == std::string::npos ? m_Diagnostics : m_Diagnostics.substr(at + 1); }
  Prepared Prepare(const std::vector<std::string>& argv, const std::function<bool()>& cancel, const std::string& diagnosticsDir = "/tmp")
  {
    m_Error.clear();
    if (!Eligible(argv)) { Failure("Desk command is not eligible"); return Prepared::Failed; }
    if (m_Pid && (m_Command[0] != argv[0] || m_Command[1] != argv[1])) { Stop(); m_Error.clear(); }
    if (!m_Pid && !Start(argv, diagnosticsDir)) return Prepared::Failed;
    const auto id = std::to_string(++m_Id);
    HarborTrace::Event("desk.host.prepare", {{"child_pid", std::to_string(m_Pid)}, {"visit_id", id}, {"view", argv[4]}});
    if (!Send("prepare\t" + id + "\t" + argv[4])) return Failed();
    std::string reply;
    // This is process observation and explicit input cancellation, with no elapsed-time death inference.
    while (true)
    {
      if (cancel())
      {
        // Nothing has yielded and no action can have started. Cancellation need not wait for a stuck preparer.
        Send("cancel\t" + id); Stop(); return Prepared::Cancelled;
      }
      if (!Receive(reply)) return Failed();
      if (reply.empty()) continue;
      if (reply == "rejected\t" + id) { Failure("Office rejected this view; see diagnostics"); return Prepared::Rejected; }
      if (reply == "ready\t" + id) return Prepared::Ready;
      Failure("Unexpected preparation reply: " + reply); return Failed();
    }
  }

  bool Visit(const std::function<bool()>& terminate = [] { return false; })
  {
    const auto id = std::to_string(m_Id);
    HarborTrace::Event("desk.host.yield", {{"child_pid", std::to_string(m_Pid)}, {"visit_id", id}});
    if (!Send("yield\t" + id)) { Stop(); return false; }
    std::string reply;
    do {
      if (terminate()) { Failure("Harbor termination requested"); Stop(); return false; }
      if (!Receive(reply)) { Stop(); return false; }
    } while (reply.empty());
    if (reply == "released\t" + id + "\tok") { HarborTrace::Event("desk.host.released", {{"child_pid", std::to_string(m_Pid)}, {"visit_id", id}}); return true; }
    // Even a released failure is retired so no uncertain controller/action is reused.
    Failure(reply == "released\t" + id + "\tfailed" ? "Office view failed; see diagnostics" : "Unexpected release reply: " + reply);
    Stop(); return false;
  }
  void Stop()
  {
    if (m_Fd >= 0) { close(m_Fd); m_Fd = -1; }
    if (m_Pid > 0)
    {
      // Only our unreaped direct child can still have this PID. Warm mode starts no nested tty child.
      kill(m_Pid, SIGKILL);
      int status = 0;
      pid_t reaped;
      do { reaped = waitpid(m_Pid, &status, 0); } while (reaped < 0 && errno == EINTR);
      if (reaped == m_Pid) RecordStatus(status);
      else Failure("waitpid: " + std::string(strerror(errno)));
      m_Pid = 0;
    }
    m_Buffer.clear();
  }
private:
  Prepared Failed() { Stop(); return Prepared::Failed; }
  void Failure(const std::string& detail)
  {
    HarborTrace::Event("desk.host.failed", {{"child_pid", std::to_string(m_Pid)}, {"visit_id", std::to_string(m_Id)}, {"diagnostics_file", DiagnosticName()}});
    if (m_Error.empty()) m_Error = detail;
    if (m_Diagnostics.empty()) return;
    const int fd = open(m_Diagnostics.c_str(), O_WRONLY | O_APPEND);
    if (fd < 0) return;
    const auto text = "\nHarbor: " + detail + "\n";
    size_t at = 0;
    while (at < text.size())
    {
      const auto n = write(fd, text.data() + at, text.size() - at);
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0) break;
      at += size_t(n);
    }
    close(fd);
  }
  void RecordStatus(int status)
  {
    HarborTrace::Event("desk.host.child.result", {{"child_pid", std::to_string(m_Pid)}, {"wait_status", std::to_string(status)}});
    const auto detail = WIFEXITED(status) ? "Office child exited " + std::to_string(WEXITSTATUS(status)) : WIFSIGNALED(status) ? "Office child signal " + std::to_string(WTERMSIG(status)) : "Office child status " + std::to_string(status);
    Failure(detail + " (wait status " + std::to_string(status) + ")");
  }
  bool Start(const std::vector<std::string>& argv, const std::string& diagnosticsDir)
  {
    auto pattern = diagnosticsDir + "/harbor-desk-error-XXXXXX";
    std::vector<char> name(pattern.begin(), pattern.end()); name.push_back(0);
    const int diagnosticFd = mkstemp(name.data());
    if (diagnosticFd < 0) { HarborTrace::Os("desk.diagnostics.create.failed", errno); m_Diagnostics.clear(); Failure("Create Desk diagnostics: " + std::string(strerror(errno))); return false; }
    m_Diagnostics = name.data(); fcntl(diagnosticFd, F_SETFD, FD_CLOEXEC);
    fcntl(diagnosticFd, F_SETFL, O_APPEND); // child stderr and parent status records must never overwrite one another
    for (const auto& check : std::vector<std::pair<std::string, int>>{{argv[0], X_OK}, {argv[1], R_OK}})
      if (access(check.first.c_str(), check.second) != 0) { HarborTrace::Os("desk.host.access.failed", errno, {{"target", check.second == X_OK ? "executable" : "script"}}); Failure("access " + check.first + ": " + strerror(errno)); close(diagnosticFd); return false; }
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) { HarborTrace::Os("desk.host.socketpair.failed", errno); Failure("socketpair: " + std::string(strerror(errno))); close(diagnosticFd); return false; }
    // Keep the reserved inherited descriptor distinct even in a descriptor-heavy Harbor process.
    for (int& fd : pair) if (fd == 198) { const int moved = fcntl(fd, F_DUPFD, 199); if (moved < 0) { Failure("duplicate private socket: " + std::string(strerror(errno))); close(pair[0]); close(pair[1]); close(diagnosticFd); return false; } close(fd); fd = moved; }
    fcntl(pair[0], F_SETFD, FD_CLOEXEC); fcntl(pair[1], F_SETFD, FD_CLOEXEC);
    posix_spawn_file_actions_t files;
    posix_spawn_file_actions_init(&files);
    posix_spawn_file_actions_adddup2(&files, diagnosticFd, STDERR_FILENO);
    if (diagnosticFd != STDERR_FILENO && diagnosticFd != 198) posix_spawn_file_actions_addclose(&files, diagnosticFd);
    posix_spawn_file_actions_adddup2(&files, pair[1], 198);
    posix_spawn_file_actions_addclose(&files, pair[0]);
    if (pair[1] != 198) posix_spawn_file_actions_addclose(&files, pair[1]);
    std::vector<std::string> env;
    for (char** item = environ; *item; ++item)
      if (std::string(*item).find("HARBOR_DESK_HOST_FD=") != 0) env.emplace_back(*item);
    env.emplace_back("HARBOR_DESK_HOST_FD=198");
    std::vector<char*> envp, args;
    for (auto& item : env) envp.push_back(const_cast<char*>(item.c_str()));
    envp.push_back(nullptr);
    for (const auto& arg : argv) args.push_back(const_cast<char*>(arg.c_str()));
    args.push_back(nullptr);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    sigset_t defaults, empty;
    sigemptyset(&defaults); sigemptyset(&empty);
    for (int sig : { SIGINT, SIGQUIT, SIGTSTP, SIGPIPE }) sigaddset(&defaults, sig);
    posix_spawnattr_setsigdefault(&attr, &defaults); posix_spawnattr_setsigmask(&attr, &empty);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK);
    pid_t pid = 0;
    const int result = posix_spawn(&pid, args[0], &files, &attr, args.data(), envp.data());
    posix_spawnattr_destroy(&attr); posix_spawn_file_actions_destroy(&files); close(pair[1]); close(diagnosticFd);
    if (result != 0) { HarborTrace::Os("desk.host.spawn.failed", result); Failure("posix_spawn " + argv[0] + ": " + strerror(result) + " (" + std::to_string(result) + ")"); close(pair[0]); return false; }
    HarborTrace::Event("desk.host.spawned", {{"child_pid", std::to_string(pid)}});
    m_Pid = pid; m_Fd = pair[0]; m_Command = argv; return true;
  }
  bool Send(const std::string& line)
  {
    const auto bytes = line + "\n";
    size_t at = 0;
    while (at < bytes.size())
    {
      // Do not alter Harbor's process-wide SIGPIPE disposition or expose it to a broken peer.
#ifdef MSG_NOSIGNAL
      const auto sent = send(m_Fd, bytes.data() + at, bytes.size() - at, MSG_NOSIGNAL);
#else
      int yes = 1; setsockopt(m_Fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes));
      const auto sent = send(m_Fd, bytes.data() + at, bytes.size() - at, 0);
#endif
      if (sent < 0 && errno == EINTR) continue;
      if (sent <= 0) { Failure("Private socket send: " + std::string(strerror(errno))); return false; }
      at += static_cast<size_t>(sent);
    }
    return true;
  }
  bool Receive(std::string& line)
  {
    line.clear();
    auto end = m_Buffer.find('\n');
    if (end != std::string::npos)
    {
      line = m_Buffer.substr(0, end); m_Buffer.erase(0, end + 1);
      if (line.empty()) { Failure("Empty Office private reply"); return false; }
      return true;
    }
    int status = 0;
    const auto exited = waitpid(m_Pid, &status, WNOHANG);
    if (exited == m_Pid) { RecordStatus(status); m_Pid = 0; return false; }
    if (exited < 0 && errno != EINTR) { Failure("waitpid: " + std::string(strerror(errno))); return false; }
    struct pollfd p = { m_Fd, POLLIN, 0 };
    const auto ready = poll(&p, 1, 20);
    if (ready < 0) { if (errno == EINTR) return true; Failure("Private socket poll: " + std::string(strerror(errno))); return false; }
    if (!ready) return true;
    char bytes[256];
    const auto got = read(m_Fd, bytes, sizeof(bytes));
    if (got < 0 && errno == EINTR) return true;
    if (got <= 0) { Failure(got == 0 ? "Office private socket EOF" : "Private socket read: " + std::string(strerror(errno))); return false; }
    m_Buffer.append(bytes, static_cast<size_t>(got));
    if (m_Buffer.size() > 1024) { Failure("Office private reply exceeded 1024 bytes"); return false; }
    for (char c : m_Buffer) if (c != '\t' && c != '\n' && (c < 32 || c > 126)) { Failure("Malformed Office private reply"); return false; }
    return true;
  }
  pid_t m_Pid = 0;
  int m_Fd = -1;
  unsigned long long m_Id = 0;
  std::string m_Buffer;
  std::string m_Error;
  std::string m_Diagnostics;
  std::vector<std::string> m_Command;
};
