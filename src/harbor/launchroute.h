#pragma once
#include <algorithm>
#include <cstdint>
#include "forumid.h"
#include "trace.h"
#include <stdexcept>
#include <string>
#include <vector>

namespace HarborRoute
{
struct Target
{
  std::string chat, topic, profile, conversation, message;
  bool Present() const { return !chat.empty(); }
};
inline void Validate(Target& target)
{
  if (!target.Present())
  {
    if (!target.topic.empty() || !target.profile.empty() || !target.message.empty())
      throw std::invalid_argument("--open-telegram-topic and --open-profile require --open-telegram-chat.");
    return;
  }
  auto decimal = [](const std::string& value, bool signedValue) {
    const size_t begin = signedValue && !value.empty() && value[0] == '-' ? 1 : 0;
    return begin < value.size() && std::all_of(value.begin() + begin, value.end(), [](char c) { return c >= '0' && c <= '9'; });
  };
  if (!decimal(target.chat, true)) throw std::invalid_argument("--open-telegram-chat requires a nonzero signed decimal Telegram chat ID.");
  int64_t chat;
  try { chat = std::stoll(target.chat); }
  catch (...) { throw std::invalid_argument("--open-telegram-chat is outside the signed 64-bit range."); }
  if (!chat) throw std::invalid_argument("--open-telegram-chat cannot be zero.");
  if (!target.message.empty())
  {
    int64_t message = 0;
    try { if (decimal(target.message, false)) message = std::stoll(target.message); } catch (...) { }
    if (message <= 0 || message > INT32_MAX) throw std::invalid_argument("--open-telegram-message requires a positive 32-bit decimal message ID.");
  }
  target.conversation = StrUtil::NumToHex(chat);
  if (!target.topic.empty())
  {
    int64_t topic = 0;
    try { if (decimal(target.topic, false)) topic = std::stoll(target.topic); } catch (...) { }
    if (topic <= 0 || topic > INT32_MAX) throw std::invalid_argument("--open-telegram-topic requires a positive 32-bit decimal topic ID.");
    target.conversation = TelegramForum::ConversationId(chat, int32_t(topic));
  }
}
inline std::string DuetProfile(const std::string& profile)
{
  return "duet-" + HarborTrace::Token(profile);
}
inline std::string SelectProfile(const Target& target, const std::vector<std::string>& profiles, bool demo)
{
  std::vector<std::string> eligible;
  for (const auto& profile : profiles)
    if (profile.rfind("Telegram_", 0) == 0 || (demo && profile.rfind("HarborDemo_", 0) == 0)) eligible.push_back(profile);
  if (!target.profile.empty())
  {
    if (getenv("DUET_HARBOR_CATALOGUE") && target.profile.rfind("duet-", 0) == 0)
    {
      std::vector<std::string> matches;
      std::copy_if(eligible.begin(), eligible.end(), std::back_inserter(matches), [&](const std::string& profile) {
        return DuetProfile(profile) == target.profile;
      });
      if (matches.size() != 1)
        throw std::invalid_argument("Requested Telegram profile is unavailable. No other conversation was opened.");
      return matches.front();
    }
    if (std::find(eligible.begin(), eligible.end(), target.profile) == eligible.end())
      throw std::invalid_argument("Requested Telegram profile is unavailable. No other conversation was opened.");
    return target.profile;
  }
  if (eligible.size() != 1)
    throw std::invalid_argument("Requested conversation needs exactly one Telegram profile, or --open-profile naming the account. No other conversation was opened.");
  return eligible.front();
}
}
