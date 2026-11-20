#pragma once
#include "forumid.h"
#include <cstdint>
#include <string>
#include <td/telegram/td_api.h>

namespace TelegramForum
{
inline std::string
ActionConversation(const td::td_api::updateChatAction& action, bool includeTopics)
{
  if (includeTopics && action.topic_id_ && action.topic_id_->get_id() == td::td_api::messageTopicForum::ID)
    return ConversationId(action.chat_id_, static_cast<const td::td_api::messageTopicForum&>(*action.topic_id_).forum_topic_id_);
  return StrUtil::NumToHex(action.chat_id_);
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
