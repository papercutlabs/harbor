#pragma once
#include "strutil.h"
#include <cstdint>
#include <string>

namespace TelegramForum
{
inline int32_t
TopicNumber(const std::string& id)
{
  auto split = id.find('#');
  if (split == std::string::npos)
    return 0;
  try
  {
    auto suffix = id.substr(split + 1);
    size_t consumed = 0;
    auto value = std::stoll(suffix, &consumed);
    return consumed == suffix.size() && value > 0 && value <= INT32_MAX ? int32_t(value) : 0;
  }
  catch (...)
  {
    return 0;
  }
}
inline int64_t
ChatNumber(const std::string& id)
{
  return StrUtil::NumFromHex<int64_t>(id.substr(0, id.find('#')));
}
inline std::string
ConversationId(int64_t chat, int32_t topic)
{
  return StrUtil::NumToHex(chat) + "#" + std::to_string(topic);
}
}
