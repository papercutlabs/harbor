#pragma once
#include "protocol.h"
#include <atomic>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

// An offline implementation of the same Protocol boundary used by TDLib.
class HarborDemo : public Protocol
{
public:
  ~HarborDemo() override { Logout(); }
  static std::string GetName() { return "HarborDemo"; }
  static std::string GetSetupMessage() { return "Offline Harbor demo"; }
  static std::string GetLibName() { return ""; }
  static std::string GetCreateFunc() { return ""; }
  std::string GetProfileId() const override { return m_Id; }
  std::string GetProfileDisplayName() const override { return "demo"; }
  bool HasFeature(ProtocolFeature f) const override { return f == FeatureAutoGetContactsOnLogin; }
  bool IsGroupChat(const std::string& id) const override { return id == "studio"; }
  std::string GetSelfId() const override { return "self"; }
  bool SetupProfile(const std::string&, std::string&) override { return false; }
  bool LoadProfile(const std::string&, const std::string& id) override
  {
    m_Id = id;
    Seed();
    return true;
  }
  bool CloseProfile() override { return true; }
  bool Login() override;
  bool Logout() override;
  void SendRequest(std::shared_ptr<RequestMessage>) override;
  void SetMessageHandler(const std::function<void(std::shared_ptr<ServiceMessage>)>& fn) override
  {
    m_Handler = fn;
  }

private:
  void Seed();
  void SeedRich(const std::function<void(const std::string&, const std::string&, const std::string&, int,
                                         const std::string&, const std::string&)>& add);
  void Record(const std::string&);
  void Later(int milliseconds, std::shared_ptr<ServiceMessage>);
  std::vector<std::thread> m_Delayed;
  std::mutex m_RecordMutex;
  void Emit(std::shared_ptr<ServiceMessage> message)
  {
    if (m_Handler)
      m_Handler(message);
  }
  std::string m_Id = "HarborDemo_local";
  int m_NextId = 100;
  int m_SendCount = 0;
  std::thread m_LiveThread;
  std::atomic<bool> m_Stopping{ false };
  std::map<std::string, std::vector<ChatMessage>> m_Messages;
  std::function<void(std::shared_ptr<ServiceMessage>)> m_Handler;
};
