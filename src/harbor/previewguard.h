#pragma once
// One per-user ordering boundary shared by native Preview dispatch and Office Focus.
// The persistent inode is never unlinked; closing the last inherited fd releases it.
#include "fileutil.h"
#include "wire.h"
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <string>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
namespace HarborPreviewGuard
{
constexpr int EffectLimitMs = 15000;
inline int LimitMs()
{
  const char* test = getenv("HARBOR_TEST_FOCUS_LIMIT_MS");
  return test && getenv("HARBOR_DEMO_FIXTURES") ? std::max(20, std::min(EffectLimitMs, atoi(test))) : EffectLimitMs;
}
inline std::string Path()
{
  const char* home = getenv("HOME");
  return home && *home ? std::string(home) + "/.office/desk/preview-focus.lock" : "";
}
class Lease
{
public:
  int fd = -1;
  bool busy = false;
  std::string error;
  explicit Lease(bool exclusive)
  {
    const auto path = Path();
    if (path.empty()) { error = "HOME is unset; start Harbor from the user's normal terminal."; return; }
    const auto office = std::string(getenv("HOME")) + "/.office";
    for (const auto& directory : { office, office + "/desk" })
      if (mkdir(directory.c_str(), 0700) && errno != EEXIST)
      { error = "Could not create the Focus/Preview directory: " + std::string(strerror(errno)); return; }
    fd = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) { error = "Could not open the Focus/Preview guard: " + std::string(strerror(errno)); return; }
    struct stat st{};
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid())
    { error = "The Focus/Preview guard is not a regular file owned by this user."; close(fd); fd = -1; return; }
    if (flock(fd, (exclusive ? LOCK_EX : LOCK_SH) | LOCK_NB))
    {
      busy = errno == EWOULDBLOCK || errno == EAGAIN;
      if (!busy) error = "Could not acquire the Focus/Preview guard: " + std::string(strerror(errno));
      close(fd); fd = -1;
    }
  }
  ~Lease() { if (fd >= 0) close(fd); } // no LOCK_UN: an effect child may still own this description
  Lease(const Lease&) = delete;
  Lease& operator=(const Lease&) = delete;
};
inline bool ReadAll(int fd, char* data, size_t size, bool& empty)
{
  size_t at = 0;
  while (at < size)
  {
    const auto n = read(fd, data + at, size - at);
    if (n > 0) at += size_t(n);
    else if (n < 0 && errno == EINTR) continue;
    else { empty = at == 0 && n == 0; return false; }
  }
  empty = false; return true;
}
inline void Forward(FILE* from, int to)
{
  rewind(from); char buffer[16384]; size_t n;
  while ((n = fread(buffer, 1, sizeof(buffer), from)) != 0)
    for (size_t at = 0; at < n;)
    {
      const auto written = write(to, buffer + at, n - at);
      if (written > 0) at += size_t(written);
      else if (written < 0 && errno == EINTR) continue;
      else return;
    }
}
// Early internal exec stage: the kernel alarm survives exec, including guard-parent death.
inline int Effect(int argc, char** argv)
{
  if (argc < 4 || std::string(argv[3]) != "/usr/bin/osascript")
  { fprintf(stderr, "Focus effect is an internal child; invoke harbor --focus-guard instead.\n"); return 2; }
  char* end = nullptr; const long fd = strtol(argv[2], &end, 10);
  struct stat held{}, named{};
  if (!end || *end || fd < 3 || fstat(int(fd), &held) || stat(Path().c_str(), &named) ||
      held.st_dev != named.st_dev || held.st_ino != named.st_ino || flock(int(fd), LOCK_EX | LOCK_NB))
  { fprintf(stderr, "Focus effect has no inherited guard; invoke --focus-guard first.\n"); return 2; }
  signal(SIGALRM, SIG_DFL);
  sigset_t unblocked; sigemptyset(&unblocked); sigaddset(&unblocked, SIGALRM); sigprocmask(SIG_UNBLOCK, &unblocked, nullptr);
  const int ms = LimitMs(); struct itimerval timer{};
  timer.it_value.tv_sec = ms / 1000; timer.it_value.tv_usec = (ms % 1000) * 1000;
  if (setitimer(ITIMER_REAL, &timer, nullptr)) { perror("Could not bound the Focus effect"); return 1; }
  const char* target = "/usr/bin/osascript";
  if (getenv("HARBOR_DEMO_FIXTURES") && getenv("HARBOR_TEST_FOCUS_EFFECT")) target = getenv("HARBOR_TEST_FOCUS_EFFECT");
  execv(target, argv + 3);
  perror("Could not execute the Focus effect"); return 1;
}
inline int Run()
{
  signal(SIGPIPE, SIG_IGN);
  Lease lease(true);
  if (lease.busy) { printf("BUSY\n"); return 75; }
  if (lease.fd < 0) { fprintf(stderr, "%s\n", lease.error.c_str()); return 1; }
  printf("READY\n"); fflush(stdout);
  std::string header(4, '\0'); bool empty = false;
  if (!ReadAll(STDIN_FILENO, header.data(), header.size(), empty))
  { if (!empty) fprintf(stderr, "Incomplete Focus argv frame; send one complete frame.\n"); return empty ? 0 : 2; }
  HarborWire::Reader length(header); const auto size = length.U32();
  if (size < 8 || size > 65536) { fprintf(stderr, "Invalid Focus argv frame size; send at most 64 KiB.\n"); return 2; }
  std::string payload(size, '\0');
  if (!ReadAll(STDIN_FILENO, payload.data(), payload.size(), empty))
  { fprintf(stderr, "Incomplete Focus argv frame; send one complete frame.\n"); return 2; }
  HarborWire::Reader reader(payload); const auto count = reader.Count(); std::vector<std::string> args;
  if (!count || count > 128) { fprintf(stderr, "Invalid Focus argv count; send 1 to 128 arguments.\n"); return 2; }
  for (uint32_t i = 0; i < count; ++i) args.push_back(reader.S());
  if (!reader.Done() || args[0] != "/usr/bin/osascript" ||
      std::any_of(args.begin(), args.end(), [](const auto& arg) { return arg.find('\0') != std::string::npos; }))
  { fprintf(stderr, "Invalid Focus argv; send exact framed arguments for /usr/bin/osascript without NULs.\n"); return 2; }
  FILE* output = tmpfile(); FILE* errors = tmpfile();
  if (!output || !errors) { perror("Could not capture the Focus effect"); if (output) fclose(output); if (errors) fclose(errors); return 1; }
  fcntl(fileno(output), F_SETFD, FD_CLOEXEC); fcntl(fileno(errors), F_SETFD, FD_CLOEXEC);
  const auto self = FileUtil::GetSelfPath();
  std::vector<std::string> command{ self, "--focus-guard-effect", std::to_string(lease.fd) };
  command.insert(command.end(), args.begin(), args.end());
  std::vector<char*> pointers; for (auto& arg : command) pointers.push_back(arg.data()); pointers.push_back(nullptr);
  posix_spawn_file_actions_t actions; posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_adddup2(&actions, fileno(output), STDOUT_FILENO);
  posix_spawn_file_actions_adddup2(&actions, fileno(errors), STDERR_FILENO);
  posix_spawnattr_t attributes; posix_spawnattr_init(&attributes);
  posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP); posix_spawnattr_setpgroup(&attributes, 0);
  fcntl(lease.fd, F_SETFD, 0); // inherited by the effect even if this holder dies
  pid_t child = -1; const int started = posix_spawn(&child, self.c_str(), &actions, &attributes, pointers.data(), environ);
  fcntl(lease.fd, F_SETFD, FD_CLOEXEC);
  posix_spawnattr_destroy(&attributes); posix_spawn_file_actions_destroy(&actions);
  if (started) { fprintf(stderr, "Could not start the Focus effect: %s\n", strerror(started)); fclose(output); fclose(errors); return 1; }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(LimitMs());
  bool timedOut = false; int status = 0;
  int waited = 0;
  while ((waited = waitpid(child, &status, WNOHANG)) != child)
  {
    if (waited < 0)
    {
      if (errno == EINTR) continue;
      const int problem = errno; kill(-child, SIGKILL);
      fprintf(stderr, "Could not wait for the Focus effect: %s\n", strerror(problem));
      Forward(output, STDOUT_FILENO); Forward(errors, STDERR_FILENO); fclose(output); fclose(errors); return 1;
    }
    if (std::chrono::steady_clock::now() >= deadline)
    {
      timedOut = true; kill(-child, SIGKILL);
      while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
      break;
    }
    poll(nullptr, 0, 10);
  }
  Forward(output, STDOUT_FILENO); Forward(errors, STDERR_FILENO); fclose(output); fclose(errors);
  if (timedOut || (WIFSIGNALED(status) && WTERMSIG(status) == SIGALRM))
  { kill(-child, SIGKILL); fprintf(stderr, "Focus effect exceeded its execution bound (%d ms).\n", LimitMs()); return 124; }
  return WIFEXITED(status) ? WEXITSTATUS(status) : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 1;
}
}
