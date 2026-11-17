#pragma once
#include "protocol.h"
#include "richtextutil.h"
#include "strutil.h"

namespace TelegramReply
{
inline void Convert(const td::td_api::messageReplyToMessage& reply, ChatMessage& out)
{
  namespace api = td::td_api;
  out.quotedId = StrUtil::NumToHex(reply.message_id_);
  out.quotedChatId = reply.chat_id_ ? StrUtil::NumToHex(reply.chat_id_) : "?";
  if (reply.quote_ && reply.quote_->text_)
  {
    out.quotedText = reply.quote_->text_->text_;
    out.quotedRichText = TelegramRichText::Encode(*reply.quote_->text_);
  }
  if (!reply.origin_) return;
  switch (reply.origin_->get_id())
  {
    case api::messageOriginUser::ID:
      out.quotedSender = StrUtil::NumToHex(static_cast<const api::messageOriginUser&>(*reply.origin_).sender_user_id_);
      break;
    case api::messageOriginHiddenUser::ID:
      out.quotedSenderName = static_cast<const api::messageOriginHiddenUser&>(*reply.origin_).sender_name_;
      break;
    case api::messageOriginChat::ID:
    {
      const auto& origin = static_cast<const api::messageOriginChat&>(*reply.origin_);
      out.quotedSender = StrUtil::NumToHex(origin.sender_chat_id_);
      out.quotedSenderName = origin.author_signature_;
      break;
    }
    case api::messageOriginChannel::ID:
    {
      const auto& origin = static_cast<const api::messageOriginChannel&>(*reply.origin_);
      out.quotedSender = StrUtil::NumToHex(origin.chat_id_);
      out.quotedSenderName = origin.author_signature_;
      break;
    }
  }
}
}
