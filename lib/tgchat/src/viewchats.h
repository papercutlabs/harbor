// viewchats.h
//
// Per-view current-chat subscriptions for a shared session. Pure logic so it can be
// tested without TDLib.

#pragma once

#include <cstdint>
#include <map>

// Moves view p_View to chat p_ChatId (0 = no chat / view gone). Sets p_ToClose to a chat
// that no view uses any more and p_ToOpen to a chat that was not open before; each is 0 when
// no TDLib transition is needed.
inline void ViewChatTransition(std::map<uint32_t, int64_t>& p_ViewChats,
                               std::map<int64_t, int>& p_OpenRefs,
                               uint32_t p_View, int64_t p_ChatId,
                               int64_t& p_ToClose, int64_t& p_ToOpen)
{
  p_ToClose = 0;
  p_ToOpen = 0;
  auto it = p_ViewChats.find(p_View);
  const int64_t prev = (it != p_ViewChats.end()) ? it->second : 0;
  if (prev == p_ChatId) return;

  if (prev != 0)
  {
    auto ref = p_OpenRefs.find(prev);
    if ((ref != p_OpenRefs.end()) && (--ref->second <= 0))
    {
      p_OpenRefs.erase(ref);
      p_ToClose = prev;
    }
  }

  if (p_ChatId != 0)
  {
    if (p_OpenRefs[p_ChatId]++ == 0)
    {
      p_ToOpen = p_ChatId;
    }
    p_ViewChats[p_View] = p_ChatId;
  }
  else
  {
    p_ViewChats.erase(p_View);
  }
}
