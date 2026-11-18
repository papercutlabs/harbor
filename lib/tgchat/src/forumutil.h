#pragma once
#include "forumid.h"
#include <cstdint>
#include <string>
#include <td/telegram/td_api.h>

namespace TelegramForum
{
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
