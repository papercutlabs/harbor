#include "forumutil.h"
#include <cassert>
#include <iostream>
int
main()
{
  using namespace TelegramForum;
  const int64_t chat = -1001234567890LL;
  const auto parent = StrUtil::NumToHex(chat), topic = ConversationId(chat, 421);
  assert(ChatNumber(topic) == chat && TopicNumber(topic) == 421);
  assert(TopicNumber(parent) == 0 && TopicNumber(parent + "#-1") == 0);
  assert(TopicNumber(parent + "#421oops") == 0 && TopicNumber(parent + "#999999999999") == 0);
  auto history = HistoryQuery(ChatNumber(topic), 123456, 0, 50, topic);
  assert(history->get_id() == td::td_api::getForumTopicHistory::ID);
  auto& query = static_cast<td::td_api::getForumTopicHistory&>(*history);
  assert(query.chat_id_ == chat && query.forum_topic_id_ == 421 && query.from_message_id_ == 123456 &&
         query.limit_ == 50);
  td::td_api::updateChatAction action;
  action.chat_id_ = chat;
  action.topic_id_ = td::td_api::make_object<td::td_api::messageTopicForum>(421);
  assert(ActionConversation(action, true) == topic);
  assert(ActionConversation(action, false) == parent);
  action.topic_id_ = nullptr;
  assert(ActionConversation(action, true) == parent);
  auto send = SendTopic(topic);
  assert(send && send->get_id() == td::td_api::messageTopicForum::ID);
  assert(static_cast<td::td_api::messageTopicForum&>(*send).forum_topic_id_ == 421);
  assert(!SendTopic(parent));
  assert(HistoryQuery(chat, 0, 0, 50, parent)->get_id() == td::td_api::getChatHistory::ID);
  std::cout << "PASS actual TDLib forum history/send requests keep the parent chat and topic IDs distinct\n";
}
