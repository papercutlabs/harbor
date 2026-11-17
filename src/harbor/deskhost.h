#pragma once
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <functional>
#include <poll.h>
#include <spawn.h>
#include <string>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

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
  Prepared Prepare(const std::vector<std::string>& argv, const std::function<bool()>& cancel)
  {
    if (!Eligible(argv)) return Prepared::Failed;
    if (m_Pid && (m_Command[0] != argv[0] || m_Command[1] != argv[1])) Stop();
    if (!m_Pid && !Start(argv)) return Prepared::Failed;
    const auto id = std::to_string(++m_Id);
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
      if (reply == "rejected\t" + id) return Prepared::Rejected;
      if (reply == "ready\t" + id) return Prepared::Ready;
      return Failed();
    }
  }

  bool Visit()
  {
    const auto id = std::to_string(m_Id);
    if (!Send("yield\t" + id)) { Stop(); return false; }
    std::string reply;
    do { if (!Receive(reply)) { Stop(); return false; } } while (reply.empty());
    if (reply == "released\t" + id + "\tok") return true;
    // Even a released failure is retired so no uncertain controller/action is reused.
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
      while (waitpid(m_Pid, &status, 0) < 0 && errno == EINTR) {}
      m_Pid = 0;
    }
    m_Buffer.clear();
  }
private:
  Prepared Failed() { Stop(); return Prepared::Failed; }
  bool Start(const std::vector<std::string>& argv)
  {
    if (access(argv[0].c_str(), X_OK) != 0 || access(argv[1].c_str(), R_OK) != 0) return false;
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) return false;
    // Keep the reserved inherited descriptor distinct even in a descriptor-heavy Harbor process.
    for (int& fd : pair) if (fd == 198) { const int moved = fcntl(fd, F_DUPFD, 199); if (moved < 0) { close(pair[0]); close(pair[1]); return false; } close(fd); fd = moved; }
    fcntl(pair[0], F_SETFD, FD_CLOEXEC); fcntl(pair[1], F_SETFD, FD_CLOEXEC);
    posix_spawn_file_actions_t files;
    posix_spawn_file_actions_init(&files);
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
    posix_spawnattr_destroy(&attr); posix_spawn_file_actions_destroy(&files); close(pair[1]);
    if (result != 0) { close(pair[0]); return false; }
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
      if (sent <= 0) return false;
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
      return !line.empty();
    }
    int status = 0;
    const auto exited = waitpid(m_Pid, &status, WNOHANG);
    if (exited == m_Pid) { m_Pid = 0; return false; }
    if (exited < 0 && errno != EINTR) return false;
    struct pollfd p = { m_Fd, POLLIN, 0 };
    const auto ready = poll(&p, 1, 20);
    if (ready < 0) return errno == EINTR;
    if (!ready) return true;
    char bytes[256];
    const auto got = read(m_Fd, bytes, sizeof(bytes));
    if (got < 0 && errno == EINTR) return true;
    if (got <= 0) return false;
    m_Buffer.append(bytes, static_cast<size_t>(got));
    if (m_Buffer.size() > 1024) return false;
    for (char c : m_Buffer) if (c != '\t' && c != '\n' && (c < 32 || c > 126)) return false;
    return true;
  }
  pid_t m_Pid = 0;
  int m_Fd = -1;
  unsigned long long m_Id = 0;
  std::string m_Buffer;
  std::vector<std::string> m_Command;
};
