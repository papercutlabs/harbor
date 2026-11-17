#pragma once
// Private same-user wire between Harbor windows and their shared session backend.
// Frame: u32 little-endian payload length | u8 kind | payload. Requests and notifies are
// encoded field by field with exact consumption; unknown types, truncation and trailing
// bytes are faults that close only the offending connection.
#include "protocol.h"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace HarborWire
{
// Largest legitimate frame: a 50-message history page whose messages each carry text,
// rich text and quote data of several hundred KiB is far below this. Larger frames are
// rejected, never truncated.
static const uint32_t MaxFrame = 64u << 20;

enum Kind : uint8_t
{
  KindWelcome = 'W', // backend -> window: profiles (id, display name), connId
  KindRequest = 'Q', // window -> backend
  KindEvent = 'E', // backend -> window
  KindLend = 'L', // backend -> window: suspend and pass terminal
  KindLent = 'T', // window -> backend: terminal fds attached (SCM_RIGHTS)
  KindReturn = 'R', // backend -> window: terminal returned, resume
  KindEnded = 'X', // backend -> window: session ended for this window (reason text)
};

class Writer
{
public:
  std::string buf;
  bool ok = true;
  void U8(uint8_t v) { if (!ok || buf.size() >= MaxFrame) { ok = false; return; } buf.push_back((char)v); }
  void U32(uint32_t v) { for (int i = 0; i < 4; ++i) U8((uint8_t)(v >> (8 * i))); }
  void U64(uint64_t v) { for (int i = 0; i < 8; ++i) U8((uint8_t)(v >> (8 * i))); }
  void I64(int64_t v) { U64((uint64_t)v); }
  void I32(int32_t v) { U32((uint32_t)v); }
  void B(bool v) { U8(v ? 1 : 0); }
  void S(const std::string& v)
  {
    if (!ok || v.size() > MaxFrame || buf.size() > MaxFrame - 4 || v.size() > MaxFrame - 4 - buf.size())
    { ok = false; return; }
    U32((uint32_t)v.size()); buf += v;
  }
};

class Reader
{
public:
  Reader(const std::string& p_Buf) : m_Buf(p_Buf) { }
  bool ok = true;
  bool Done() const { return ok && m_Pos == m_Buf.size(); }
  uint8_t U8()
  {
    if (!ok || m_Pos + 1 > m_Buf.size()) { ok = false; return 0; }
    return (uint8_t)m_Buf[m_Pos++];
  }
  uint32_t U32() { uint32_t v = 0; for (int i = 0; i < 4; ++i) v |= (uint32_t)U8() << (8 * i); return v; }
  uint64_t U64() { uint64_t v = 0; for (int i = 0; i < 8; ++i) v |= (uint64_t)U8() << (8 * i); return v; }
  int64_t I64() { return (int64_t)U64(); }
  int32_t I32() { return (int32_t)U32(); }
  bool B()
  {
    uint8_t v = U8();
    if (v > 1) ok = false;
    return v == 1;
  }
  std::string S()
  {
    uint32_t n = U32();
    if (!ok || n > m_Buf.size() - m_Pos) { ok = false; return std::string(); }
    std::string v = m_Buf.substr(m_Pos, n);
    m_Pos += n;
    return v;
  }
  // element count bounded by remaining bytes (each element needs at least one byte)
  uint32_t Count()
  {
    uint32_t n = U32();
    if (!ok || n > m_Buf.size() - m_Pos) { ok = false; return 0; }
    return n;
  }

private:
  const std::string& m_Buf;
  size_t m_Pos = 0;
};

// Aggregate guards: adding a field to one of these structs changes its member count and
// breaks this structured binding at compile time until the codec below is updated.
inline void GuardChatMessage(const ChatMessage& m)
{
  auto& [a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16, a17, a18, a19, a20, a21] = m;
  (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7; (void)a8; (void)a9; (void)a10;
  (void)a11; (void)a12; (void)a13; (void)a14; (void)a15; (void)a16; (void)a17; (void)a18; (void)a19; (void)a20; (void)a21;
}
inline void GuardChatInfo(const ChatInfo& c)
{
  auto& [a1, a2, a3, a4, a5, a6, a7, a8, a9, a10] = c;
  (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7; (void)a8; (void)a9; (void)a10;
}
inline void GuardContactInfo(const ContactInfo& c)
{
  auto& [a1, a2, a3, a4, a5] = c;
  (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
}
inline void GuardReactions(const Reactions& r)
{
  auto& [a1, a2, a3, a4, a5] = r;
  (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
}

inline void Put(Writer& w, const Reactions& r)
{
  w.B(r.needConsolidationWithCache);
  w.B(r.updateCountBasedOnSender);
  w.B(r.replaceCount);
  w.U32((uint32_t)r.senderEmojis.size());
  for (auto& e : r.senderEmojis) { w.S(e.first); w.S(e.second); }
  w.U32((uint32_t)r.emojiCounts.size());
  for (auto& e : r.emojiCounts) { w.S(e.first); w.U64(e.second); }
}
inline void Get(Reader& r, Reactions& v)
{
  v.needConsolidationWithCache = r.B();
  v.updateCountBasedOnSender = r.B();
  v.replaceCount = r.B();
  for (uint32_t i = 0, n = r.Count(); r.ok && i < n; ++i) { auto k = r.S(); v.senderEmojis[k] = r.S(); }
  for (uint32_t i = 0, n = r.Count(); r.ok && i < n; ++i) { auto k = r.S(); v.emojiCounts[k] = (size_t)r.U64(); }
}

inline void Put(Writer& w, const ChatMessage& m)
{
  w.S(m.id); w.S(m.senderId); w.S(m.text); w.S(m.quotedId); w.S(m.quotedText); w.S(m.quotedSender);
  w.S(m.quotedChatId); w.S(m.quotedSenderName); w.S(m.quotedRichText); w.S(m.fileInfo); w.S(m.richText);
  w.S(m.link);
  w.U32((uint32_t)m.mentions.size());
  for (auto& e : m.mentions) { w.S(e.first); w.S(e.second); }
  Put(w, m.reactions);
  w.I64(m.timeSent);
  w.B(m.isOutgoing); w.B(m.isRead); w.B(m.isEdited); w.B(m.isDeleted); w.B(m.isPinned); w.B(m.hasMention);
}
inline void Get(Reader& r, ChatMessage& m)
{
  m.id = r.S(); m.senderId = r.S(); m.text = r.S(); m.quotedId = r.S(); m.quotedText = r.S();
  m.quotedSender = r.S(); m.quotedChatId = r.S(); m.quotedSenderName = r.S(); m.quotedRichText = r.S();
  m.fileInfo = r.S(); m.richText = r.S(); m.link = r.S();
  for (uint32_t i = 0, n = r.Count(); r.ok && i < n; ++i) { auto k = r.S(); m.mentions[k] = r.S(); }
  Get(r, m.reactions);
  m.timeSent = r.I64();
  m.isOutgoing = r.B(); m.isRead = r.B(); m.isEdited = r.B(); m.isDeleted = r.B(); m.isPinned = r.B();
  m.hasMention = r.B();
}
inline void Put(Writer& w, const ChatInfo& c)
{
  w.S(c.id); w.B(c.isUnread); w.B(c.isUnreadMention); w.B(c.isMuted); w.B(c.isPinned); w.B(c.isArchived);
  w.I64(c.lastMessageTime); w.I64(c.actualLastMessageTime); w.S(c.lastMessageText); w.B(c.lastMessageNative);
}
inline void Get(Reader& r, ChatInfo& c)
{
  c.id = r.S(); c.isUnread = r.B(); c.isUnreadMention = r.B(); c.isMuted = r.B(); c.isPinned = r.B();
  c.isArchived = r.B(); c.lastMessageTime = r.I64(); c.actualLastMessageTime = r.I64();
  c.lastMessageText = r.S(); c.lastMessageNative = r.B();
}
inline void Put(Writer& w, const ContactInfo& c)
{
  w.S(c.id); w.S(c.name); w.S(c.phone); w.B(c.isSelf); w.B(c.isAlias);
}
inline void Get(Reader& r, ContactInfo& c)
{
  c.id = r.S(); c.name = r.S(); c.phone = r.S(); c.isSelf = r.B(); c.isAlias = r.B();
}
template<typename T>
inline void PutVec(Writer& w, const std::vector<T>& v)
{
  w.U32((uint32_t)v.size());
  for (auto& e : v) Put(w, e);
}
template<typename T>
inline void GetVec(Reader& r, std::vector<T>& v)
{
  for (uint32_t i = 0, n = r.Count(); r.ok && i < n; ++i) { T e; Get(r, e); v.push_back(e); }
}
inline DownloadFileAction GetAction(Reader& r)
{
  uint8_t a = r.U8();
  if (a > DownloadFileActionSave) r.ok = false;
  return (DownloadFileAction)a;
}

// Requests Harbor issues. Anything else (SetupProfile, Reinit, DeferNotify, ...) is never
// accepted from a window.
inline bool EncodeRequest(const std::shared_ptr<RequestMessage>& p, std::string& out)
{
  Writer w;
  const MessageType t = p->GetMessageType();
  w.U32((uint32_t)t);
  w.U64(p->requestId);
  switch (t)
  {
    case GetContactsRequestType: break;
    case GetChatsRequestType:
    {
      auto q = std::static_pointer_cast<GetChatsRequest>(p);
      w.B(q->includeForumTopics);
      w.U32((uint32_t)q->chatIds.size());
      // Canonical order: chatIds is an unordered_set.
      std::vector<std::string> ids(q->chatIds.begin(), q->chatIds.end());
      std::sort(ids.begin(), ids.end());
      for (auto& id : ids) w.S(id);
      break;
    }
    case GetMessageRequestType:
    {
      auto q = std::static_pointer_cast<GetMessageRequest>(p);
      w.S(q->chatId); w.S(q->msgId); w.B(q->cached); w.B(q->replyPreview);
      break;
    }
    case GetMessagesRequestType:
    {
      auto q = std::static_pointer_cast<GetMessagesRequest>(p);
      w.S(q->chatId); w.S(q->fromMsgId); w.I32(q->limit);
      break;
    }
    case SendMessageRequestType:
    {
      auto q = std::static_pointer_cast<SendMessageRequest>(p);
      w.S(q->chatId); Put(w, q->chatMessage);
      break;
    }
    case EditMessageRequestType:
    {
      auto q = std::static_pointer_cast<EditMessageRequest>(p);
      w.S(q->chatId); w.S(q->msgId); Put(w, q->chatMessage);
      break;
    }
    case DeleteMessageRequestType:
    {
      auto q = std::static_pointer_cast<DeleteMessageRequest>(p);
      w.S(q->chatId); w.S(q->senderId); w.S(q->msgId);
      break;
    }
    case SetStatusRequestType:
      w.B(std::static_pointer_cast<SetStatusRequest>(p)->isOnline);
      break;
    case DownloadFileRequestType:
    {
      auto q = std::static_pointer_cast<DownloadFileRequest>(p);
      w.S(q->chatId); w.S(q->msgId); w.S(q->fileId); w.U8((uint8_t)q->downloadFileAction);
      break;
    }
    case SetCurrentChatRequestType:
      w.S(std::static_pointer_cast<SetCurrentChatRequest>(p)->chatId);
      break;
    case GetAvailableReactionsRequestType:
    {
      auto q = std::static_pointer_cast<GetAvailableReactionsRequest>(p);
      w.S(q->chatId); w.S(q->msgId);
      break;
    }
    case SendReactionRequestType:
    {
      auto q = std::static_pointer_cast<SendReactionRequest>(p);
      w.S(q->chatId); w.S(q->senderId); w.S(q->msgId); w.S(q->emoji); w.S(q->prevEmoji);
      break;
    }
    case SearchMessagesRequestType:
    {
      auto q = std::static_pointer_cast<SearchMessagesRequest>(p);
      w.S(q->chatId); w.S(q->query); w.B(q->pinned); w.I32(q->limit); w.I64(q->token);
      break;
    }
    default:
      return false;
  }
  if (!w.ok || w.buf.size() >= MaxFrame) return false;
  out.swap(w.buf);
  return true;
}

inline std::shared_ptr<RequestMessage> DecodeRequest(const std::string& in)
{
  Reader r(in);
  const MessageType t = (MessageType)r.U32();
  const uint64_t requestId = r.U64();
  std::shared_ptr<RequestMessage> p;
  switch (t)
  {
    case GetContactsRequestType: p = std::make_shared<GetContactsRequest>(); break;
    case GetChatsRequestType:
    {
      auto q = std::make_shared<GetChatsRequest>();
      q->includeForumTopics = r.B();
      for (uint32_t i = 0, n = r.Count(); r.ok && i < n; ++i) q->chatIds.insert(r.S());
      p = q;
      break;
    }
    case GetMessageRequestType:
    {
      auto q = std::make_shared<GetMessageRequest>();
      q->chatId = r.S(); q->msgId = r.S(); q->cached = r.B(); q->replyPreview = r.B();
      p = q;
      break;
    }
    case GetMessagesRequestType:
    {
      auto q = std::make_shared<GetMessagesRequest>();
      q->chatId = r.S(); q->fromMsgId = r.S(); q->limit = r.I32();
      p = q;
      break;
    }
    case SendMessageRequestType:
    {
      auto q = std::make_shared<SendMessageRequest>();
      q->chatId = r.S(); Get(r, q->chatMessage);
      p = q;
      break;
    }
    case EditMessageRequestType:
    {
      auto q = std::make_shared<EditMessageRequest>();
      q->chatId = r.S(); q->msgId = r.S(); Get(r, q->chatMessage);
      p = q;
      break;
    }
    case DeleteMessageRequestType:
    {
      auto q = std::make_shared<DeleteMessageRequest>();
      q->chatId = r.S(); q->senderId = r.S(); q->msgId = r.S();
      p = q;
      break;
    }
    case SetStatusRequestType:
    {
      auto q = std::make_shared<SetStatusRequest>();
      q->isOnline = r.B();
      p = q;
      break;
    }
    case DownloadFileRequestType:
    {
      auto q = std::make_shared<DownloadFileRequest>();
      q->chatId = r.S(); q->msgId = r.S(); q->fileId = r.S(); q->downloadFileAction = GetAction(r);
      p = q;
      break;
    }
    case SetCurrentChatRequestType:
    {
      auto q = std::make_shared<SetCurrentChatRequest>();
      q->chatId = r.S();
      p = q;
      break;
    }
    case GetAvailableReactionsRequestType:
    {
      auto q = std::make_shared<GetAvailableReactionsRequest>();
      q->chatId = r.S(); q->msgId = r.S();
      p = q;
      break;
    }
    case SendReactionRequestType:
    {
      auto q = std::make_shared<SendReactionRequest>();
      q->chatId = r.S(); q->senderId = r.S(); q->msgId = r.S(); q->emoji = r.S(); q->prevEmoji = r.S();
      p = q;
      break;
    }
    case SearchMessagesRequestType:
    {
      auto q = std::make_shared<SearchMessagesRequest>();
      q->chatId = r.S(); q->query = r.S(); q->pinned = r.B(); q->limit = r.I32(); q->token = r.I64();
      p = q;
      break;
    }
    default:
      return nullptr;
  }
  if (!r.Done()) return nullptr;
  p->requestId = requestId;
  return p;
}

// Notifies Harbor consumes. ProtocolUiControlNotify never crosses: the backend answers it.
inline bool EncodeNotify(const std::shared_ptr<ServiceMessage>& p, std::string& out)
{
  Writer w;
  const MessageType t = p->GetMessageType();
  w.U32((uint32_t)t);
  w.U64(p->requestId);
  w.S(p->profileId);
  switch (t)
  {
    case ConnectNotifyType:
      w.B(std::static_pointer_cast<ConnectNotify>(p)->success);
      break;
    case NewContactsNotifyType:
    {
      auto n = std::static_pointer_cast<NewContactsNotify>(p);
      w.B(n->fullSync); PutVec(w, n->contactInfos);
      break;
    }
    case NewChatsNotifyType:
    {
      auto n = std::static_pointer_cast<NewChatsNotify>(p);
      w.B(n->success); PutVec(w, n->chatInfos);
      break;
    }
    case NewMessagesNotifyType:
    {
      auto n = std::static_pointer_cast<NewMessagesNotify>(p);
      w.B(n->success); w.S(n->chatId); PutVec(w, n->chatMessages); w.S(n->fromMsgId); w.S(n->previewMsgId);
      w.B(n->cached); w.B(n->sequence);
      break;
    }
    case SendMessageNotifyType:
    {
      auto n = std::static_pointer_cast<SendMessageNotify>(p);
      w.B(n->success); w.B(n->pending); w.S(n->chatId); Put(w, n->chatMessage);
      break;
    }
    case DeleteMessageNotifyType:
    {
      auto n = std::static_pointer_cast<DeleteMessageNotify>(p);
      w.B(n->success); w.S(n->chatId); w.S(n->msgId); w.B(n->isOutgoing);
      break;
    }
    case ReceiveTypingNotifyType:
    {
      auto n = std::static_pointer_cast<ReceiveTypingNotify>(p);
      w.S(n->chatId); w.S(n->userId); w.B(n->isTyping);
      break;
    }
    case NewMessageStatusNotifyType:
    {
      auto n = std::static_pointer_cast<NewMessageStatusNotify>(p);
      w.S(n->chatId); w.S(n->msgId); w.B(n->isRead);
      break;
    }
    case NewMessageFileNotifyType:
    {
      auto n = std::static_pointer_cast<NewMessageFileNotify>(p);
      w.S(n->chatId); w.S(n->msgId); w.S(n->fileInfo); w.U8((uint8_t)n->downloadFileAction);
      break;
    }
    case RequestAppExitNotifyType: break;
    case NewMessageReactionsNotifyType:
    {
      auto n = std::static_pointer_cast<NewMessageReactionsNotify>(p);
      w.S(n->chatId); w.S(n->msgId); Put(w, n->reactions);
      break;
    }
    case AvailableReactionsNotifyType:
    {
      auto n = std::static_pointer_cast<AvailableReactionsNotify>(p);
      w.S(n->chatId); w.S(n->msgId);
      w.U32((uint32_t)n->emojis.size());
      for (auto& e : n->emojis) w.S(e);
      w.B(n->success);
      break;
    }
    case SearchMessagesNotifyType:
    {
      auto n = std::static_pointer_cast<SearchMessagesNotify>(p);
      w.B(n->success); w.S(n->error); w.S(n->chatId); w.S(n->query); w.B(n->pinned); w.I64(n->token);
      w.I32(n->totalCount); PutVec(w, n->chatMessages);
      break;
    }
    default:
      return false; // not consumed by Harbor windows
  }
  if (!w.ok || w.buf.size() >= MaxFrame) return false;
  out.swap(w.buf);
  return true;
}

inline std::shared_ptr<ServiceMessage> DecodeNotify(const std::string& in)
{
  Reader r(in);
  const MessageType t = (MessageType)r.U32();
  const uint64_t requestId = r.U64();
  const std::string profileId = r.S();
  std::shared_ptr<ServiceMessage> p;
  switch (t)
  {
    case ConnectNotifyType:
    {
      auto n = std::make_shared<ConnectNotify>(profileId);
      n->success = r.B();
      p = n;
      break;
    }
    case NewContactsNotifyType:
    {
      auto n = std::make_shared<NewContactsNotify>(profileId);
      n->fullSync = r.B(); GetVec(r, n->contactInfos);
      p = n;
      break;
    }
    case NewChatsNotifyType:
    {
      auto n = std::make_shared<NewChatsNotify>(profileId);
      n->success = r.B(); GetVec(r, n->chatInfos);
      p = n;
      break;
    }
    case NewMessagesNotifyType:
    {
      auto n = std::make_shared<NewMessagesNotify>(profileId);
      n->success = r.B(); n->chatId = r.S(); GetVec(r, n->chatMessages); n->fromMsgId = r.S();
      n->previewMsgId = r.S(); n->cached = r.B(); n->sequence = r.B();
      p = n;
      break;
    }
    case SendMessageNotifyType:
    {
      auto n = std::make_shared<SendMessageNotify>(profileId);
      n->success = r.B(); n->pending = r.B(); n->chatId = r.S(); Get(r, n->chatMessage);
      p = n;
      break;
    }
    case DeleteMessageNotifyType:
    {
      auto n = std::make_shared<DeleteMessageNotify>(profileId);
      n->success = r.B(); n->chatId = r.S(); n->msgId = r.S(); n->isOutgoing = r.B();
      p = n;
      break;
    }
    case ReceiveTypingNotifyType:
    {
      auto n = std::make_shared<ReceiveTypingNotify>(profileId);
      n->chatId = r.S(); n->userId = r.S(); n->isTyping = r.B();
      p = n;
      break;
    }
    case NewMessageStatusNotifyType:
    {
      auto n = std::make_shared<NewMessageStatusNotify>(profileId);
      n->chatId = r.S(); n->msgId = r.S(); n->isRead = r.B();
      p = n;
      break;
    }
    case NewMessageFileNotifyType:
    {
      auto n = std::make_shared<NewMessageFileNotify>(profileId);
      n->chatId = r.S(); n->msgId = r.S(); n->fileInfo = r.S(); n->downloadFileAction = GetAction(r);
      p = n;
      break;
    }
    case RequestAppExitNotifyType: p = std::make_shared<RequestAppExitNotify>(profileId); break;
    case NewMessageReactionsNotifyType:
    {
      auto n = std::make_shared<NewMessageReactionsNotify>(profileId);
      n->chatId = r.S(); n->msgId = r.S(); Get(r, n->reactions);
      p = n;
      break;
    }
    case AvailableReactionsNotifyType:
    {
      auto n = std::make_shared<AvailableReactionsNotify>(profileId);
      n->chatId = r.S(); n->msgId = r.S();
      for (uint32_t i = 0, c = r.Count(); r.ok && i < c; ++i) n->emojis.insert(r.S());
      n->success = r.B();
      p = n;
      break;
    }
    case SearchMessagesNotifyType:
    {
      auto n = std::make_shared<SearchMessagesNotify>(profileId);
      n->success = r.B(); n->error = r.S(); n->chatId = r.S(); n->query = r.S(); n->pinned = r.B();
      n->token = r.I64(); n->totalCount = r.I32(); GetVec(r, n->chatMessages);
      p = n;
      break;
    }
    default:
      return nullptr;
  }
  if (!r.Done()) return nullptr;
  p->requestId = requestId;
  return p;
}

inline std::string Frame(Kind k, const std::string& payload)
{
  if (payload.size() >= MaxFrame) return std::string();
  Writer w;
  w.U32((uint32_t)payload.size() + 1);
  w.U8((uint8_t)k);
  w.buf += payload;
  return w.buf;
}

// Incremental frame splitter. Returns false on a malformed or oversized frame.
inline bool NextFrame(std::string& buf, Kind& kind, std::string& payload, bool& have)
{
  have = false;
  if (buf.size() < 4) return true;
  uint32_t len = (uint8_t)buf[0] | ((uint32_t)(uint8_t)buf[1] << 8) | ((uint32_t)(uint8_t)buf[2] << 16) |
                 ((uint32_t)(uint8_t)buf[3] << 24);
  if (len == 0 || len > MaxFrame) return false;
  if (buf.size() < 4 + (size_t)len) return true;
  kind = (Kind)(uint8_t)buf[4];
  payload = buf.substr(5, len - 1);
  buf.erase(0, 4 + (size_t)len);
  have = true;
  return true;
}
}
