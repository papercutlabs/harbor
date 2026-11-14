#pragma once
#include "strutil.h"
#include <cstdint>
#include <string>
#include <td/telegram/td_api.h>

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
inline td::td_api::object_ptr<td::td_api::Function>
HistoryQuery(int64_t chat, int64_t from, int32_t offset, int32_t limit, const std::string& view)
{
  auto topic = TopicNumber(view);
  if (topic)
    return td::td_api::make_object<td::td_api::getForumTopicHistory>(chat, topic, from, offset, limit);
  return td::td_api::make_object<td::td_api::getChatHistory>(chat, from, offset, limit, false);
}
inline td::td_api::object_ptr<td::td_api::MessageTopic>
SendTopic(const std::string& view)
{
  auto topic = TopicNumber(view);
  if (topic)
    return td::td_api::make_object<td::td_api::messageTopicForum>(topic);
  return nullptr;
}
}
