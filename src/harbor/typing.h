#pragma once
#include <cstdint>
#include <map>
#include <string>

// Transient provider activity. Each Conversation owns one instance: never persisted as draft/state.
// TDLib coalesces unchanged actions and owns the inactivity timeout. A second receiver timeout
// would clear continuously renewed activity because those renewals do not produce notifications.
class HarborTyping
{
public:
  void Update(const std::string& user, bool active, int64_t now)
  {
    if (user.empty()) return;
    if (active)
    {
      if (!m_Users.count(user)) m_Users[user] = now;
    }
    else m_Users.erase(user);
  }
  void Complete(const std::string& user, int64_t sent)
  {
    auto found = m_Users.find(user);
    // Telegram timestamps have one-second precision. A history page received during typing
    // cannot end current activity just because it contains an older message by the same user.
    if (found != m_Users.end() && sent >= found->second - 1000) m_Users.erase(found);
  }
  void Clear() { m_Users.clear(); }
  bool Active() const { return !m_Users.empty(); }
  static int Phase(int64_t now) { return int((now / 240) % 3); }
private:
  std::map<std::string, int64_t> m_Users;
};
