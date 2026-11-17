#pragma once
// One shared session per Harbor configuration directory.
//
// The backend is the same harbor binary started with the internal argv marker
// --session-backend <fd>. It runs today's native initialisation (directory and profile locks,
// log, config, message cache, TDLib) and serves identical window processes over a private
// same-user Unix socket. It is born with its spawning window already connected through <fd>
// and exits when its connected-window count drops to zero.
//
// Each window is the ordinary Harbor UI whose only protocols are HarborRemote proxies.
#include "protocol.h"
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace HarborSession
{
// Exact internal argv marker of the backend entry. Not a user-facing operation.
static const char* const BackendArg = "--session-backend";

std::string SocketPath(const std::string& p_AppDir);
// Empty when p_Path fits sockaddr_un; otherwise a diagnosis naming the limit.
std::string SocketPathProblem(const std::string& p_Path);

struct Profile
{
  std::string id;
  std::string displayName;
};

// Window side ---------------------------------------------------------------------------------

struct Attachment
{
  int fd = -1;
  std::vector<Profile> profiles;
  std::string error; // backend start failure, shown to the user
  std::string pending; // bytes read after the welcome (for example replayed events)
};

// Connect to the running backend, or spawn one with an inherited connection. Serialised by an
// advisory lock so simultaneous starts produce exactly one backend. p_BackendArgs are passed
// to the spawned backend after the marker (for example -d DIR, --demo).
Attachment Attach(const std::string& p_AppDir, const std::string& p_Self,
                  const std::vector<std::string>& p_BackendArgs);

// Lowest free per-view state slot, held by an advisory lock for the life of the process.
// Slot 0 keeps the existing harbor-state filename.
int AcquireSlot(const std::string& p_AppDir);

class Connection;

// Protocol proxy for one profile served by the backend.
class Remote : public Protocol
{
public:
  Remote(std::shared_ptr<Connection> p_Connection, const Profile& p_Profile);
  std::string GetProfileId() const override { return m_Profile.id; }
  std::string GetProfileDisplayName() const override { return m_Profile.displayName; }
  bool HasFeature(ProtocolFeature) const override { return false; }
  bool IsGroupChat(const std::string&) const override { return false; }
  std::string GetSelfId() const override { return std::string(); }
  bool SetupProfile(const std::string&, std::string&) override { return false; }
  bool LoadProfile(const std::string&, const std::string&) override { return true; }
  bool CloseProfile() override { return true; }
  bool Login() override;
  bool Logout() override { return true; }
  void SendRequest(std::shared_ptr<RequestMessage> p_Request) override;
  void SetMessageHandler(const std::function<void(std::shared_ptr<ServiceMessage>)>& p_Handler) override;

private:
  std::shared_ptr<Connection> m_Connection;
  Profile m_Profile;
};

// Window end of the socket: one reader thread, frame writes under a mutex.
class Connection
{
public:
  Connection(int p_Fd, const std::string& p_Pending);
  ~Connection();
  void Start(); // begins reading; replays events for all registered handlers
  void Stop();
  bool Send(const std::string& p_ProfileId, std::shared_ptr<RequestMessage> p_Request);
  void SetHandler(const std::string& p_ProfileId, const std::function<void(std::shared_ptr<ServiceMessage>)>& p_Handler);
  // Called by the window once it has left ncurses for a lend: passes its terminal to the backend.
  void Lent();
  bool Ended() const { return m_Ended; }
  std::string EndReason();

private:
  void ReadLoop();
  void Dispatch(std::shared_ptr<ServiceMessage>);
  void End(const std::string& p_Reason);
  int m_Fd = -1;
  std::string m_Pending;
  std::thread m_Reader;
  std::mutex m_WriteMutex;
  std::mutex m_Mutex;
  std::map<std::string, std::function<void(std::shared_ptr<ServiceMessage>)>> m_Handlers;
  std::atomic<bool> m_Ended{ false };
  std::atomic<bool> m_Stopping{ false };
  std::string m_EndReason;
  uint32_t m_Seq = 0;
  // Sends whose outcome has not arrived; on session loss they are reported as failed
  // (draft restored for recovery) and never replayed.
  std::map<uint64_t, std::shared_ptr<SendMessageNotify>> m_Unknown;
};

// Backend side --------------------------------------------------------------------------------

// Serves windows until none is connected. p_InheritedFd is the spawning window's connection.
// p_Protocols are loaded but not yet logged in; the backend sets their handlers (and the
// message cache handler through p_SetCacheHandler) and logs them in. Returns the process exit
// status. The optional shutdown lock is kept by main until native locks are released.
int RunBackend(int p_InheritedFd, const std::string& p_AppDir,
               const std::vector<std::shared_ptr<Protocol>>& p_Protocols,
               const std::function<void(const std::function<void(std::shared_ptr<ServiceMessage>)>&)>& p_SetCacheHandler, int* p_ShutdownLock = nullptr,
               const std::function<bool()>& p_ShouldStop = std::function<bool()>());

// Send a startup failure over the inherited connection so the spawning window can show it.
void FailBackend(int p_InheritedFd, const std::string& p_Message);
}
