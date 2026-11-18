#pragma once
#include "trace.h"
#include "protocol.h"
namespace HarborTrace
{
inline const char* Operation(MessageType type)
{
  switch (type)
  {
#define HARBOR_TRACE_REQUEST(name, text) case name##RequestType: return text;
    HARBOR_TRACE_REQUEST(GetContacts, "contacts.load")
    HARBOR_TRACE_REQUEST(GetChats, "chats.load")
    HARBOR_TRACE_REQUEST(GetMessage, "message.load")
    HARBOR_TRACE_REQUEST(GetMessages, "history.load")
    HARBOR_TRACE_REQUEST(SendMessage, "message.send")
    HARBOR_TRACE_REQUEST(EditMessage, "message.edit")
    HARBOR_TRACE_REQUEST(DownloadFile, "attachment.download")
    HARBOR_TRACE_REQUEST(MarkMessageRead, "message.mark_read")
    HARBOR_TRACE_REQUEST(DeleteMessage, "message.delete")
    HARBOR_TRACE_REQUEST(DeleteChat, "chat.delete")
    HARBOR_TRACE_REQUEST(ArchiveChat, "chat.archive")
    HARBOR_TRACE_REQUEST(PinChat, "chat.pin")
    HARBOR_TRACE_REQUEST(PinMessage, "message.pin")
    HARBOR_TRACE_REQUEST(CreateChat, "chat.create")
    HARBOR_TRACE_REQUEST(SetCurrentChat, "chat.open")
    HARBOR_TRACE_REQUEST(GetAvailableReactions, "reactions.load")
    HARBOR_TRACE_REQUEST(SendReaction, "reaction.send")
    HARBOR_TRACE_REQUEST(Reinit, "profile.reinitialize")
    HARBOR_TRACE_REQUEST(FindMessage, "message.find")
    HARBOR_TRACE_REQUEST(SearchMessages, "messages.search")
    HARBOR_TRACE_REQUEST(GetGroupMembers, "members.load")
#undef HARBOR_TRACE_REQUEST
    // Typing, status polls, background deferrals and unread-reaction refreshes are not actions.
    default: return nullptr;
  }
}
inline void Request(const char* stage, const std::shared_ptr<RequestMessage>& request, uint32_t connection = 0, int profile = -1)
{
  if (const auto op = Operation(request->GetMessageType()))
    Event(stage, {{"operation", op}, {"request_id", Number(request->requestId & 0xffffffffULL)},
      {"connection_id", Number(connection)}, {"profile_index", std::to_string(profile)}, {"type", Number(request->GetMessageType())}});
}
inline void Result(const char* stage, const std::shared_ptr<ServiceMessage>& n, uint32_t connection = 0, int profile = -1)
{
  const auto type = n->GetMessageType();
  if (!n->requestId && type != ConnectNotifyType) return; // no unsolicited conversation content
  std::string outcome = "received";
  switch (type)
  {
    case SendMessageNotifyType:
    { auto send = std::static_pointer_cast<SendMessageNotify>(n); outcome = send->pending ? "pending" : send->success ? "success" : "failed"; break; }
#define HARBOR_TRACE_RESULT(name) case name##NotifyType: outcome = std::static_pointer_cast<name##Notify>(n)->success ? "success" : "failed"; break;
    HARBOR_TRACE_RESULT(Connect)
    HARBOR_TRACE_RESULT(NewChats)
    HARBOR_TRACE_RESULT(NewMessages)
    HARBOR_TRACE_RESULT(DeleteMessage)
    HARBOR_TRACE_RESULT(DeleteChat)
    HARBOR_TRACE_RESULT(CreateChat)
    HARBOR_TRACE_RESULT(MarkMessageRead)
#undef HARBOR_TRACE_RESULT
    case SendTypingNotifyType: case SetStatusNotifyType: return;
    default: break; // delivery is known; success may not be represented by this native event
  }
  Event(stage, {{"request_id", Number(n->requestId & 0xffffffffULL)}, {"connection_id", Number(connection)},
    {"profile_index", std::to_string(profile)}, {"type", Number(type)}, {"outcome", outcome}});
}
}
