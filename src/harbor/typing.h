#pragma once
#include <cstdint>
#include <map>
#include <string>

// Transient provider activity. Each Conversation owns one instance: never persisted as draft/state.
class HarborTyping
{
public:
  void Update(const std::string& user, bool active, int64_t now)
  {
    if (user.empty()) return;
    if (active)
    {
      auto found = m_Users.find(user);
      if (found == m_Users.end() || found->second.until <= now) m_Users[user] = {now, now + 6000};
      else found->second.until = now + 6000;
    }
    else m_Users.erase(user);
  }
  void Complete(const std::string& user, int64_t sent)
  {
    auto found = m_Users.find(user);
    // Telegram timestamps have one-second precision. A history page received during typing
    // cannot end current activity just because it contains an older message by the same user.
    if (found != m_Users.end() && sent >= found->second.started - 1000) m_Users.erase(found);
  }
  bool Expire(int64_t now)
  {
    bool changed = false;
    for (auto it = m_Users.begin(); it != m_Users.end();)
      if (it->second.until <= now) { it = m_Users.erase(it); changed = true; }
      else ++it;
    return changed;
  }
  bool Active() const { return !m_Users.empty(); }
  static int Phase(int64_t now) { return int((now / 240) % 3); }
private:
  struct Activity { int64_t started, until; };
  std::map<std::string, Activity> m_Users;
};
