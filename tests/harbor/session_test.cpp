// Shared-session checks: wire codec round trips, frame faults, per-view chat subscriptions and
// backend routing of request-scoped replies over real sockets with a throwaway directory.
#include "session.h"
#include "sessiongeneration.h"
#include "viewchats.h"
#include "wire.h"
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <fcntl.h>
#include <poll.h>
#include <mutex>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

using namespace HarborWire;

static ChatMessage
Full(const std::string& tag)
{
  ChatMessage m;
  m.id = tag + "-id";
  m.senderId = tag + "-sender";
  m.text = tag + " text **bold** ||spoiler||";
  m.quotedId = tag + "-qid";
  m.quotedText = tag + " quoted";
  m.quotedSender = tag + "-qsender";
  m.quotedChatId = "?";
  m.quotedSenderName = tag + " origin";
  m.quotedRichText = std::string("rich\0quote", 10);
  m.fileInfo = tag + "-fileinfo-hex";
  m.richText = std::string("rich\0text\xff", 10);
  m.link = "https://example.invalid/" + tag;
  m.mentions = { { "Display", "user1" }, { "Other", "user2" } };
  m.reactions.needConsolidationWithCache = true;
  m.reactions.updateCountBasedOnSender = true;
  m.reactions.replaceCount = true;
  m.reactions.senderEmojis = { { "You", "👍" }, { "u2", "🔥" } };
  m.reactions.emojiCounts = { { "👍", 3 }, { "🔥", 1 } };
  m.timeSent = 1700000000123;
  m.isOutgoing = false;
  m.isRead = true;
  m.isEdited = true;
  m.isDeleted = true;
  m.isPinned = true;
  m.hasMention = true;
  return m;
}

static void
SameMessage(const ChatMessage& a, const ChatMessage& b)
{
  assert(a.id == b.id && a.senderId == b.senderId && a.text == b.text);
  assert(a.quotedId == b.quotedId && a.quotedText == b.quotedText && a.quotedSender == b.quotedSender);
  assert(a.quotedChatId == b.quotedChatId && a.quotedSenderName == b.quotedSenderName);
  assert(a.quotedRichText == b.quotedRichText && a.fileInfo == b.fileInfo && a.richText == b.richText);
  assert(a.link == b.link && a.mentions == b.mentions);
  assert(a.reactions == b.reactions && a.reactions.needConsolidationWithCache == b.reactions.needConsolidationWithCache);
  assert(a.reactions.updateCountBasedOnSender == b.reactions.updateCountBasedOnSender);
  assert(a.reactions.replaceCount == b.reactions.replaceCount);
  assert(a.timeSent == b.timeSent && a.isOutgoing == b.isOutgoing && a.isRead == b.isRead);
  assert(a.isEdited == b.isEdited && a.isDeleted == b.isDeleted && a.isPinned == b.isPinned);
  assert(a.hasMention == b.hasMention);
}

static std::vector<std::shared_ptr<RequestMessage>>
Requests()
{
  std::vector<std::shared_ptr<RequestMessage>> out;
  out.push_back(std::make_shared<GetContactsRequest>());
  auto chats = std::make_shared<GetChatsRequest>();
  chats->includeForumTopics = true;
  chats->chatIds = { "a", "b#1" };
  out.push_back(chats);
  auto one = std::make_shared<GetMessageRequest>();
  one->chatId = "c"; one->msgId = "m"; one->cached = false; one->replyPreview = true;
  out.push_back(one);
  auto many = std::make_shared<GetMessagesRequest>();
  many->chatId = "c"; many->fromMsgId = "f"; many->limit = 50;
  out.push_back(many);
  auto send = std::make_shared<SendMessageRequest>();
  send->chatId = "c"; send->chatMessage = Full("send");
  out.push_back(send);
  auto edit = std::make_shared<EditMessageRequest>();
  edit->chatId = "c"; edit->msgId = "m"; edit->chatMessage = Full("edit");
  out.push_back(edit);
  auto del = std::make_shared<DeleteMessageRequest>();
  del->chatId = "c"; del->senderId = "s"; del->msgId = "m";
  out.push_back(del);
  auto status = std::make_shared<SetStatusRequest>();
  status->isOnline = true;
  out.push_back(status);
  auto download = std::make_shared<DownloadFileRequest>();
  download->chatId = "c"; download->msgId = "m"; download->fileId = "f"; download->downloadFileAction = DownloadFileActionOpen;
  out.push_back(download);
  auto current = std::make_shared<SetCurrentChatRequest>();
  current->chatId = "c";
  out.push_back(current);
  auto available = std::make_shared<GetAvailableReactionsRequest>();
  available->chatId = "c"; available->msgId = "m";
  out.push_back(available);
  auto react = std::make_shared<SendReactionRequest>();
  react->chatId = "c"; react->senderId = "s"; react->msgId = "m"; react->emoji = "🎉"; react->prevEmoji = "👍";
  out.push_back(react);
  auto search = std::make_shared<SearchMessagesRequest>();
  search->chatId = "c"; search->query = "q"; search->pinned = true; search->limit = 7; search->token = -5;
  out.push_back(search);
  for (size_t i = 0; i < out.size(); ++i)
    out[i]->requestId = 0x0000000700000000ULL + i + 1;
  return out;
}

static std::vector<std::shared_ptr<ServiceMessage>>
Notifies()
{
  const std::string p = "Telegram_1";
  std::vector<std::shared_ptr<ServiceMessage>> out;
  auto connect = std::make_shared<ConnectNotify>(p);
  connect->success = true;
  out.push_back(connect);
  auto contacts = std::make_shared<NewContactsNotify>(p);
  contacts->fullSync = true;
  contacts->contactInfos = { ContactInfo{ "u", "Name", "123", true, true } };
  out.push_back(contacts);
  auto chats = std::make_shared<NewChatsNotify>(p);
  chats->success = true;
  ChatInfo info{ "c", true, true, true, true, true, 5, 6, "preview ||x||", true };
  chats->chatInfos = { info };
  out.push_back(chats);
  auto history = std::make_shared<NewMessagesNotify>(p);
  history->success = true; history->chatId = "c"; history->chatMessages = { Full("h1"), Full("h2") };
  history->fromMsgId = "f"; history->previewMsgId = "pv"; history->cached = true; history->sequence = true;
  out.push_back(history);
  auto sent = std::make_shared<SendMessageNotify>(p);
  sent->success = true; sent->pending = true; sent->chatId = "c"; sent->chatMessage = Full("sent");
  out.push_back(sent);
  auto del = std::make_shared<DeleteMessageNotify>(p);
  del->success = true; del->chatId = "c"; del->msgId = "m"; del->isOutgoing = true;
  out.push_back(del);
  auto typing = std::make_shared<ReceiveTypingNotify>(p);
  typing->chatId = "c"; typing->userId = "u"; typing->isTyping = true;
  out.push_back(typing);
  auto read = std::make_shared<NewMessageStatusNotify>(p);
  read->chatId = "c"; read->msgId = "m"; read->isRead = true;
  out.push_back(read);
  auto file = std::make_shared<NewMessageFileNotify>(p);
  file->chatId = "c"; file->msgId = "m"; file->fileInfo = "hex"; file->downloadFileAction = DownloadFileActionSave;
  out.push_back(file);
  out.push_back(std::make_shared<RequestAppExitNotify>(p));
  auto reactions = std::make_shared<NewMessageReactionsNotify>(p);
  reactions->chatId = "c"; reactions->msgId = "m"; reactions->reactions = Full("r").reactions;
  out.push_back(reactions);
  auto available = std::make_shared<AvailableReactionsNotify>(p);
  available->chatId = "c"; available->msgId = "m"; available->emojis = { "👍", "🎉" }; available->success = false;
  out.push_back(available);
  auto search = std::make_shared<SearchMessagesNotify>(p);
  search->success = true; search->error = "e"; search->chatId = "c"; search->query = "q"; search->pinned = true;
  search->token = 99; search->totalCount = 2; search->chatMessages = { Full("s1") };
  out.push_back(search);
  for (size_t i = 0; i < out.size(); ++i)
    out[i]->requestId = 0x0000000900000000ULL + i + 1;
  return out;
}

// Compare original fields as well as encoded bytes: two symmetric codec omissions
// would otherwise produce equal re-encodings while losing the same field both ways.
static void RequestFields(const std::shared_ptr<RequestMessage>& original, const std::shared_ptr<RequestMessage>& decoded)
{
#define REQUEST(T) case T##Type: { auto a = std::static_pointer_cast<T>(original); auto b = std::static_pointer_cast<T>(decoded);
#define SAME(field) assert(a->field == b->field)
  switch (original->GetMessageType())
  {
    case GetContactsRequestType: break;
    REQUEST(GetChatsRequest) SAME(includeForumTopics); SAME(chatIds); break; }
    REQUEST(GetMessageRequest) SAME(chatId); SAME(msgId); SAME(cached); SAME(replyPreview); break; }
    REQUEST(GetMessagesRequest) SAME(chatId); SAME(fromMsgId); SAME(limit); break; }
    REQUEST(SendMessageRequest) SAME(chatId); SameMessage(a->chatMessage, b->chatMessage); break; }
    REQUEST(EditMessageRequest) SAME(chatId); SAME(msgId); SameMessage(a->chatMessage, b->chatMessage); break; }
    REQUEST(DeleteMessageRequest) SAME(chatId); SAME(senderId); SAME(msgId); break; }
    REQUEST(SetStatusRequest) SAME(isOnline); break; }
    REQUEST(DownloadFileRequest) SAME(chatId); SAME(msgId); SAME(fileId); SAME(downloadFileAction); break; }
    REQUEST(SetCurrentChatRequest) SAME(chatId); break; }
    REQUEST(GetAvailableReactionsRequest) SAME(chatId); SAME(msgId); break; }
    REQUEST(SendReactionRequest) SAME(chatId); SAME(senderId); SAME(msgId); SAME(emoji); SAME(prevEmoji); break; }
    REQUEST(SearchMessagesRequest) SAME(chatId); SAME(query); SAME(pinned); SAME(limit); SAME(token); break; }
    default: assert(false);
  }
#undef REQUEST
#undef SAME
}
static void NotifyFields(const std::shared_ptr<ServiceMessage>& original, const std::shared_ptr<ServiceMessage>& decoded)
{
#define NOTIFY(T) case T##Type: { auto a = std::static_pointer_cast<T>(original); auto b = std::static_pointer_cast<T>(decoded);
#define SAME(field) assert(a->field == b->field)
  switch (original->GetMessageType())
  {
    NOTIFY(ConnectNotify) SAME(success); break; }
    NOTIFY(NewContactsNotify)
      SAME(fullSync); assert(a->contactInfos.size() == b->contactInfos.size());
      const auto& x = a->contactInfos[0]; const auto& y = b->contactInfos[0];
      assert(x.id == y.id && x.name == y.name && x.phone == y.phone && x.isSelf == y.isSelf && x.isAlias == y.isAlias); break; }
    NOTIFY(NewChatsNotify)
      SAME(success); assert(a->chatInfos.size() == b->chatInfos.size());
      const auto& x = a->chatInfos[0]; const auto& y = b->chatInfos[0];
      assert(x.id == y.id && x.isUnread == y.isUnread && x.isUnreadMention == y.isUnreadMention &&
             x.isMuted == y.isMuted && x.isPinned == y.isPinned && x.isArchived == y.isArchived &&
             x.lastMessageTime == y.lastMessageTime && x.actualLastMessageTime == y.actualLastMessageTime &&
             x.lastMessageText == y.lastMessageText && x.lastMessageNative == y.lastMessageNative); break; }
    NOTIFY(NewMessagesNotify)
      SAME(success); SAME(chatId); SAME(fromMsgId); SAME(previewMsgId); SAME(cached); SAME(sequence);
      assert(a->chatMessages.size() == b->chatMessages.size());
      for (size_t i = 0; i < a->chatMessages.size(); ++i) SameMessage(a->chatMessages[i], b->chatMessages[i]); break; }
    NOTIFY(SendMessageNotify) SAME(success); SAME(pending); SAME(chatId); SameMessage(a->chatMessage, b->chatMessage); break; }
    NOTIFY(DeleteMessageNotify) SAME(success); SAME(chatId); SAME(msgId); SAME(isOutgoing); break; }
    NOTIFY(ReceiveTypingNotify) SAME(chatId); SAME(userId); SAME(isTyping); break; }
    NOTIFY(NewMessageStatusNotify) SAME(chatId); SAME(msgId); SAME(isRead); break; }
    NOTIFY(NewMessageFileNotify) SAME(chatId); SAME(msgId); SAME(fileInfo); SAME(downloadFileAction); break; }
    case RequestAppExitNotifyType: break;
    NOTIFY(NewMessageReactionsNotify)
      SAME(chatId); SAME(msgId); SAME(reactions);
      SAME(reactions.needConsolidationWithCache); SAME(reactions.updateCountBasedOnSender); SAME(reactions.replaceCount); break; }
    NOTIFY(AvailableReactionsNotify) SAME(chatId); SAME(msgId); SAME(emojis); SAME(success); break; }
    NOTIFY(SearchMessagesNotify)
      SAME(success); SAME(error); SAME(chatId); SAME(query); SAME(pinned); SAME(token); SAME(totalCount);
      assert(a->chatMessages.size() == b->chatMessages.size());
      for (size_t i = 0; i < a->chatMessages.size(); ++i) SameMessage(a->chatMessages[i], b->chatMessages[i]); break; }
    default: assert(false);
  }
#undef NOTIFY
#undef SAME
}

static void
CodecTests()
{
  size_t requests = 0, notifies = 0;
  for (auto& q : Requests())
  {
    std::string a, b;
    assert(EncodeRequest(q, a));
    auto d = DecodeRequest(a);
    assert(d && d->GetMessageType() == q->GetMessageType() && d->requestId == q->requestId);
    RequestFields(q, d);
    // Canonical byte order also remains stable.
    assert(EncodeRequest(d, b) && a == b);
    for (size_t cut = 0; cut < a.size(); ++cut)
      assert(!DecodeRequest(a.substr(0, cut)));
    assert(!DecodeRequest(a + '\0'));
    ++requests;
  }
  assert(requests == 13);
  for (auto& n : Notifies())
  {
    std::string a, b;
    assert(EncodeNotify(n, a));
    auto d = DecodeNotify(a);
    assert(d && d->GetMessageType() == n->GetMessageType() && d->requestId == n->requestId &&
           d->profileId == n->profileId);
    NotifyFields(n, d);
    assert(EncodeNotify(d, b) && a == b);
    for (size_t cut = 0; cut < a.size(); ++cut)
      assert(!DecodeNotify(a.substr(0, cut)));
    assert(!DecodeNotify(a + '\0'));
    ++notifies;
  }
  assert(notifies == 13);

  // Explicit rich, quote, file and reaction fields survive.
  auto send = std::static_pointer_cast<SendMessageRequest>(Requests()[4]);
  std::string bytes;
  assert(EncodeRequest(send, bytes));
  SameMessage(std::static_pointer_cast<SendMessageRequest>(DecodeRequest(bytes))->chatMessage, send->chatMessage);

  // Never accepted from a window.
  std::string unused;
  assert(!EncodeRequest(std::make_shared<ReinitRequest>(), unused));
  assert(!EncodeRequest(std::make_shared<DeferNotifyRequest>(), unused));
  assert(!EncodeNotify(std::make_shared<ProtocolUiControlNotify>("p"), unused));
  Writer w;
  w.U32((uint32_t)ReinitRequestType);
  w.U64(0);
  assert(!DecodeRequest(w.buf));
  Writer unknown;
  unknown.U32(0xfffff);
  unknown.U64(0);
  assert(!DecodeRequest(unknown.buf));
  // A non-canonical bool is a fault.
  Writer status;
  status.U32((uint32_t)SetStatusRequestType);
  status.U64(0);
  status.U8(2);
  assert(!DecodeRequest(status.buf));
  // A huge element count cannot allocate past the frame.
  Writer chats;
  chats.U32((uint32_t)GetChatsRequestType);
  chats.U64(0);
  chats.B(false);
  chats.U32(0xffffffffu);
  assert(!DecodeRequest(chats.buf));

  // A large legitimate history page (50 messages with long text, rich text and quotes) fits.
  auto page = std::make_shared<NewMessagesNotify>("Telegram_1");
  page->success = true;
  page->chatId = "c";
  page->sequence = true;
  for (int i = 0; i < 50; ++i)
  {
    ChatMessage m = Full("big" + std::to_string(i));
    m.text = std::string(64 * 1024, 'x');
    m.richText = std::string(96 * 1024, 'r');
    m.quotedText = std::string(16 * 1024, 'q');
    m.quotedRichText = std::string(24 * 1024, 'Q');
    page->chatMessages.push_back(m);
  }
  std::string big;
  assert(EncodeNotify(page, big));
  std::string framed = Frame(KindEvent, big);
  assert(framed.size() < MaxFrame);
  Kind kind;
  std::string payload;
  bool have = false;
  assert(NextFrame(framed, kind, payload, have) && have && kind == KindEvent && payload == big);
  auto back = std::static_pointer_cast<NewMessagesNotify>(DecodeNotify(payload));
  assert(back && back->chatMessages.size() == 50 && back->chatMessages[49].richText == page->chatMessages[49].richText);

  // Oversized and empty frames are rejected, never truncated.
  Writer over;
  over.U32(MaxFrame + 1);
  over.U8(KindEvent);
  assert(!NextFrame(over.buf, kind, payload, have));
  Writer empty;
  empty.U32(0);
  assert(!NextFrame(empty.buf, kind, payload, have));
  // A partial frame waits for more bytes.
  std::string partial = Frame(KindEvent, "abc").substr(0, 6);
  assert(NextFrame(partial, kind, payload, have) && !have);
  std::cout << "codec: 13 requests, 13 wire notifies + local UI control, faults and large page ok\n";
}

static void
ViewChatTests()
{
  std::map<uint32_t, int64_t> views;
  std::map<int64_t, int> refs;
  int64_t close = 0, open = 0;
  // Two windows in different chats: both stay open.
  ViewChatTransition(views, refs, 1, 100, close, open);
  assert(open == 100 && close == 0);
  ViewChatTransition(views, refs, 2, 200, close, open);
  assert(open == 200 && close == 0);
  assert(refs.count(100) && refs.count(200));
  // Window 2 joins chat 100: 200 closes, 100 is not reopened.
  ViewChatTransition(views, refs, 2, 100, close, open);
  assert(close == 200 && open == 0 && refs[100] == 2);
  // Window 1 leaves (disconnect): 100 stays open for window 2.
  ViewChatTransition(views, refs, 1, 0, close, open);
  assert(close == 0 && open == 0 && refs[100] == 1);
  // Last view leaves: 100 closes.
  ViewChatTransition(views, refs, 2, 0, close, open);
  assert(close == 100 && open == 0 && refs.empty() && views.empty());
  // Repeating the same chat is no transition; classic single view 0 behaves as before.
  ViewChatTransition(views, refs, 0, 5, close, open);
  ViewChatTransition(views, refs, 0, 5, close, open);
  assert(close == 0 && open == 0);
  ViewChatTransition(views, refs, 0, 6, close, open);
  assert(close == 5 && open == 6);
  std::cout << "view chats: union subscriptions ok\n";
}

// Routing over real sockets ---------------------------------------------------------------------

class Fake : public Protocol
{
public:
  std::string GetProfileId() const override { return "HarborDemo_local"; }
  std::string GetProfileDisplayName() const override { return "Fake"; }
  bool HasFeature(ProtocolFeature) const override { return false; }
  bool IsGroupChat(const std::string&) const override { return false; }
  std::string GetSelfId() const override { return "self"; }
  bool SetupProfile(const std::string&, std::string&) override { return false; }
  bool LoadProfile(const std::string&, const std::string&) override { return true; }
  bool CloseProfile() override { return true; }
  bool Login() override
  {
    auto c = std::make_shared<ConnectNotify>(GetProfileId());
    c->success = true;
    m_Handler(c);
    return true;
  }
  bool Logout() override { return true; }
  void SetMessageHandler(const std::function<void(std::shared_ptr<ServiceMessage>)>& h) override { m_Handler = h; }
  void SendRequest(std::shared_ptr<RequestMessage> r) override
  {
    {
      std::lock_guard<std::mutex> lock(m_Mutex);
      m_Seen.push_back(r);
    }
    if (r->GetMessageType() == SendMessageRequestType)
    {
      auto q = std::static_pointer_cast<SendMessageRequest>(r);
      auto n = std::make_shared<SendMessageNotify>(GetProfileId());
      n->chatId = q->chatId;
      n->chatMessage = q->chatMessage;
      n->success = ++m_Sends != 2; // the second send fails, whichever window issued it
      n->requestId = q->requestId;
      m_Handler(n);
      if (n->success)
      {
        auto live = std::make_shared<NewMessagesNotify>(GetProfileId()); // unsolicited: broadcast
        live->success = true;
        live->chatId = q->chatId;
        live->sequence = true;
        live->chatMessages = { q->chatMessage };
        m_Handler(live);
      }
    }
    else if (r->GetMessageType() == GetMessagesRequestType)
    {
      auto q = std::static_pointer_cast<GetMessagesRequest>(r);
      auto n = std::make_shared<NewMessagesNotify>(GetProfileId());
      n->success = true;
      n->chatId = q->chatId;
      n->fromMsgId = q->fromMsgId;
      n->sequence = true;
      n->requestId = q->requestId;
      m_Handler(n);
    }
  }
  void Emit(std::shared_ptr<ServiceMessage> n) { m_Handler(n); }
  std::vector<std::shared_ptr<RequestMessage>> Seen()
  {
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_Seen;
  }

private:
  std::function<void(std::shared_ptr<ServiceMessage>)> m_Handler;
  std::mutex m_Mutex;
  std::vector<std::shared_ptr<RequestMessage>> m_Seen;
  int m_Sends = 0;
};

struct Window
{
  std::shared_ptr<HarborSession::Connection> connection;
  std::mutex mutex;
  std::condition_variable cond;
  std::vector<std::shared_ptr<ServiceMessage>> events;

  void Attach(int fd)
  {
    std::string buf, payload;
    Kind kind;
    bool have = false;
    while (!have)
    {
      char chunk[4096];
      ssize_t n = recv(fd, chunk, sizeof(chunk), 0);
      assert(n > 0);
      buf.append(chunk, (size_t)n);
      assert(NextFrame(buf, kind, payload, have));
    }
    assert(kind == KindWelcome);
    connection = std::make_shared<HarborSession::Connection>(fd, buf);
    connection->SetHandler("HarborDemo_local", [this](std::shared_ptr<ServiceMessage> e)
    {
      std::lock_guard<std::mutex> lock(mutex);
      events.push_back(e);
      cond.notify_all();
    });
    connection->Start();
  }
  size_t Count(MessageType type, int success = -1)
  {
    std::lock_guard<std::mutex> lock(mutex);
    size_t n = 0;
    for (auto& e : events)
      if (e->GetMessageType() == type &&
          (success < 0 || (type == SendMessageNotifyType &&
                           std::static_pointer_cast<SendMessageNotify>(e)->success == (success == 1)) ||
           (type == NewMessagesNotifyType && (!std::static_pointer_cast<NewMessagesNotify>(e)->fromMsgId.empty()) == (success == 1))))
        ++n;
    return n;
  }
  bool WaitFor(MessageType type, size_t n, int success = -1)
  {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < until)
    {
      if (Count(type, success) >= n)
        return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::cerr << "timed out waiting for type " << type << " count " << n << "\n";
    return false;
  }
};

static int
RawConnect(const std::string& appDir)
{
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  assert(fd >= 0);
  sockaddr_un addr = {};
  addr.sun_family = AF_UNIX;
  const auto path = HarborSession::SocketPath(appDir);
  strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
  assert(connect(fd, (sockaddr*)&addr, sizeof(addr)) == 0);
  return fd;
}
static void
Closed(int fd)
{
  pollfd p = {fd, POLLIN, 0};
  assert(poll(&p, 1, 5000) == 1);
  char bytes[4096];
  while (true)
  {
    ssize_t n = recv(fd, bytes, sizeof(bytes), 0);
    if (n == 0) break;
    assert(n > 0);
    p.revents = 0;
    assert(poll(&p, 1, 5000) == 1);
  }
  close(fd);
}
static void
UnknownSendTests(bool orderly)
{
  int pair[2];
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
  Window window;
  auto conn = std::make_shared<HarborSession::Connection>(pair[0], "");
  window.connection = conn;
  conn->SetHandler("HarborDemo_local", [&window](std::shared_ptr<ServiceMessage> n)
  { std::lock_guard<std::mutex> lock(window.mutex); window.events.push_back(n); });
  conn->Start();
  std::vector<ChatMessage> originals;
  for (int i = 0; i < 2; ++i)
  {
    auto send = std::make_shared<SendMessageRequest>();
    send->chatId = "same";
    send->chatMessage = Full("unknown" + std::to_string(i));
    originals.push_back(send->chatMessage);
    assert(conn->Send("HarborDemo_local", send));
  }
  // A pending acceptance is not a final outcome, and must not erase recovery state.
  auto pending = std::make_shared<SendMessageNotify>("HarborDemo_local");
  pending->success = true; pending->pending = true; pending->requestId = 1;
  std::string encoded;
  assert(EncodeNotify(pending, encoded));
  const auto frame = Frame(KindEvent, encoded);
  assert(send(pair[1], frame.data(), frame.size(), 0) == (ssize_t)frame.size());
  assert(window.WaitFor(SendMessageNotifyType, 1, 1)); // acceptance reached the reader
  // A confirmed send must not be recovered along with the two unknown originals.
  auto confirmed = std::make_shared<SendMessageRequest>();
  confirmed->chatId = "same"; confirmed->chatMessage = Full("confirmed");
  assert(conn->Send("HarborDemo_local", confirmed));
  auto final = std::make_shared<SendMessageNotify>("HarborDemo_local");
  final->requestId = confirmed->requestId; final->success = true;
  assert(EncodeNotify(final, encoded));
  const auto done = Frame(KindEvent, encoded);
  assert(send(pair[1], done.data(), done.size(), 0) == (ssize_t)done.size());
  assert(window.WaitFor(SendMessageNotifyType, 2, 1));
  if (orderly)
  {
    conn->Stop();
    assert(!conn->Ended() && window.Count(RequestAppExitNotifyType) == 0);
    assert(!conn->Send("HarborDemo_local", confirmed)); // frozen, no replay/new request
    conn->Stop(); // finalization is idempotent
  }
  else
  {
    shutdown(pair[1], SHUT_WR);
    assert(window.WaitFor(RequestAppExitNotifyType, 1));
  }
  assert(window.Count(SendMessageNotifyType, 0) == 2);
  {
    std::lock_guard<std::mutex> lock(window.mutex);
    size_t recovered = 0;
    for (auto& e : window.events)
      if (e->GetMessageType() == SendMessageNotifyType && !std::static_pointer_cast<SendMessageNotify>(e)->success)
      {
        assert(e->requestId == recovered + 1); // each original keeps its request identity
        SameMessage(std::static_pointer_cast<SendMessageNotify>(e)->chatMessage, originals[recovered++]);
      }
    assert(recovered == 2);
  }
  std::string requests;
  char chunk[4096];
  fcntl(pair[1], F_SETFL, O_NONBLOCK);
  while (ssize_t n = recv(pair[1], chunk, sizeof(chunk), 0))
  { if (n < 0) break; requests.append(chunk, (size_t)n); }
  int sent = 0;
  while (!requests.empty())
  { Kind kind; std::string payload; bool have = false; assert(NextFrame(requests, kind, payload, have) && have && kind == KindRequest); ++sent; }
  assert(sent == 3); // two unknown originals + one confirmed send, no replay
  conn->Stop();
  close(pair[1]);
  std::cout << "unknown sends: pending acceptance + " << (orderly ? "orderly stop" : "EOF")
            << " retains full rich/quote/attachment payloads and request identities without replay\n";
}

static void
Send(Window& w, const std::string& text)
{
  auto r = std::make_shared<SendMessageRequest>();
  r->chatId = "marina";
  r->chatMessage.text = text;
  assert(w.connection->Send("HarborDemo_local", r));
}

static void
RoutingTests()
{
  // Throwaway directory only; never HOME, XDG or a real profile.
  char tmpl[] = "/tmp/harbor-session-test-XXXXXX";
  const char* dir = mkdtemp(tmpl);
  assert(dir);
  const std::string appDir = dir;
  const std::string sentinel = appDir + "/sentinel";
  { FILE* f = fopen(sentinel.c_str(), "w"); fputs("keep", f); fclose(f); }

  int pair[2];
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
  auto fake = std::make_shared<Fake>();
  int rv = -1;
  std::thread backend([&]
  {
    rv = HarborSession::RunBackend(pair[1], appDir, { fake }, [](const std::function<void(std::shared_ptr<ServiceMessage>)>&) { });
  });
  Window a;
  a.Attach(pair[0]);
  assert(a.WaitFor(ConnectNotifyType, 1));

  // Late attach through the listening socket gets the replayed ConnectNotify.
  const std::string path = HarborSession::SocketPath(appDir);
  struct stat st;
  assert(stat(path.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600);
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
  assert(connect(fd, (sockaddr*)&addr, sizeof(addr)) == 0);
  Window b;
  b.Attach(fd);
  assert(b.WaitFor(ConnectNotifyType, 1));

  // Same text from both windows; the second send fails. Exactly the failing window sees failure.
  Send(a, "ok");
  assert(a.WaitFor(SendMessageNotifyType, 1));
  Send(b, "ok");
  assert(b.WaitFor(SendMessageNotifyType, 1));
  assert(a.Count(SendMessageNotifyType, 1) == 1 && a.Count(SendMessageNotifyType, 0) == 0);
  assert(b.Count(SendMessageNotifyType, 0) == 1 && b.Count(SendMessageNotifyType, 1) == 0);
  // The successful send's live message is shared.
  assert(b.WaitFor(NewMessagesNotifyType, 1, 0) && a.WaitFor(NewMessagesNotifyType, 1, 0));
  // Exactly one native send per window request.
  size_t sends = 0;
  for (auto& r : fake->Seen())
    if (r->GetMessageType() == SendMessageRequestType)
      ++sends;
  assert(sends == 2);

  // A history page answers only its requester.
  auto page = std::make_shared<GetMessagesRequest>();
  page->chatId = "marina";
  page->fromMsgId = "older";
  page->limit = 50;
  assert(a.connection->Send("HarborDemo_local", page));
  assert(a.WaitFor(NewMessagesNotifyType, 1, 1));
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  assert(b.Count(NewMessagesNotifyType, 1) == 0);

  // Connection identities differ and are stamped in the high half.
  std::set<uint32_t> views;
  for (auto& r : fake->Seen())
    views.insert((uint32_t)(r->requestId >> 32));
  assert(views.size() == 2 && !views.count(0));

  // Every encoded notify preserves its payload and is delivered only to its target.
  // This tests the real backend wire/connection boundary, not a TDLib account.
  for (auto& n : Notifies())
  {
    if (n->GetMessageType() == RequestAppExitNotifyType) continue;
    n->profileId = "HarborDemo_local";
    n->requestId = fake->Seen().front()->requestId;
    const size_t beforeA = a.Count(n->GetMessageType()), beforeB = b.Count(n->GetMessageType());
    fake->Emit(n);
    assert(a.WaitFor(n->GetMessageType(), beforeA + 1));
    assert(b.Count(n->GetMessageType()) == beforeB);
  }

  // Faults close only their peer, including arbitrary descriptor transfers.
  for (const auto& bad : { Frame((Kind)'?', ""), Frame(KindLent, ""), std::string("\0\0\0\0", 4) })
  {
    int badFd = RawConnect(appDir);
    assert(send(badFd, bad.data(), bad.size(), 0) == (ssize_t)bad.size());
    Closed(badFd);
  }
  {
    int badFd = RawConnect(appDir), pipes[2];
    assert(pipe(pipes) == 0);
    const auto bad = Frame(KindLent, "");
    iovec io = {(void*)bad.data(), bad.size()};
    char control[CMSG_SPACE(sizeof(pipes))] = {};
    msghdr msg = {};
    msg.msg_iov = &io; msg.msg_iovlen = 1; msg.msg_control = control; msg.msg_controllen = sizeof(control);
    cmsghdr* c = CMSG_FIRSTHDR(&msg);
    c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS; c->cmsg_len = CMSG_LEN(sizeof(pipes));
    memcpy(CMSG_DATA(c), pipes, sizeof(pipes));
    assert(sendmsg(badFd, &msg, 0) == (ssize_t)bad.size());
    close(pipes[0]); close(pipes[1]);
    Closed(badFd);
  }
  std::cout << "peer validation: unknown/empty frames and unsolicited descriptors isolated\n";

  // A stopped reader has a bounded queue and cannot block healthy connections.
  int slow = RawConnect(appDir);
  auto slowPage = std::make_shared<GetMessagesRequest>();
  slowPage->requestId = 1; slowPage->chatId = "slow";
  std::string slowEncoded;
  assert(EncodeRequest(slowPage, slowEncoded));
  Writer envelope; envelope.S("HarborDemo_local"); envelope.S(slowEncoded);
  const auto requestFrame = Frame(KindRequest, envelope.buf);
  assert(send(slow, requestFrame.data(), requestFrame.size(), 0) == (ssize_t)requestFrame.size());
  uint64_t slowId = 0;
  for (int i = 0; i < 1000 && !slowId; ++i)
  {
    for (auto& q : fake->Seen())
      if (q->GetMessageType() == GetMessagesRequestType && std::static_pointer_cast<GetMessagesRequest>(q)->chatId == "slow") slowId = q->requestId;
    if (!slowId) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  assert(slowId);
  auto large = std::make_shared<NewMessagesNotify>("HarborDemo_local");
  large->requestId = slowId; large->chatId = "slow";
  ChatMessage blob; blob.text = std::string(48 * 1024 * 1024, 'x'); large->chatMessages = {blob};
  for (int i = 0; i < 4; ++i) fake->Emit(large);
  bool slowCleared = false;
  for (int i = 0; i < 1000 && !slowCleared; ++i)
  {
    for (auto& q : fake->Seen())
      if (q->GetMessageType() == SetCurrentChatRequestType && (q->requestId >> 32) == (slowId >> 32)) slowCleared = true;
    if (!slowCleared) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  assert(slowCleared);
  close(slow);
  std::cout << "slow reader: bounded queue evicts only stopped peer, healthy readers remain connected\n";

  // Close A: B keeps working and its sends still succeed.
  a.connection->Stop();
  const size_t beforeSurvivor = b.Count(SendMessageNotifyType, 1);
  Send(b, "after");
  assert(b.WaitFor(SendMessageNotifyType, beforeSurvivor + 1, 1));
  // A's departure cleared A's subscription.
  bool cleared = false;
  for (int i = 0; i < 1000 && !cleared; ++i)
  {
    for (auto& r : fake->Seen())
      if (r->GetMessageType() == SetCurrentChatRequestType &&
          std::static_pointer_cast<SetCurrentChatRequest>(r)->chatId.empty())
        cleared = true;
    if (!cleared)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  assert(cleared);

  // Last window gone: backend returns and removes its socket; unrelated files survive.
  b.connection->Stop();
  backend.join();
  assert(rv == 0);
  assert(stat(path.c_str(), &st) != 0);
  assert(stat(sentinel.c_str(), &st) == 0);
  unlink(sentinel.c_str());
  unlink((appDir + "/session.lock").c_str());
  assert(rmdir(dir) == 0); // nothing else was left behind
  std::cout << "routing: same-text ownership, history isolation, late attach, close and last-client ok\n";
}

static void
SocketPathTests()
{
  assert(HarborSession::SocketPathProblem("/tmp/x/session.sock").empty());
  const std::string longPath = "/" + std::string(200, 'd') + "/session.sock";
  const std::string problem = HarborSession::SocketPathProblem(longPath);
  assert(!problem.empty() && problem.find("-d") != std::string::npos);
  std::cout << "socket path: long directory diagnosed\n";
}

static void
GenerationTests()
{
  char directory[] = "/tmp/harbor-generation-XXXXXX";
  assert(mkdtemp(directory));
  const std::string appDir = directory, path = HarborSession::SocketPath(appDir);
  const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  assert(fd >= 0);
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  strcpy(address.sun_path, path.c_str());
  assert(bind(fd, (sockaddr*)&address, sizeof(address)) == 0);
  struct stat socket{};
  assert(lstat(path.c_str(), &socket) == 0);
  std::string token;
  assert(HarborSessionGeneration::Read(appDir, socket, token) && token.empty());
  {
    HarborSessionGeneration::Publisher owner;
    assert(owner.Publish(appDir, path));
    assert(HarborSessionGeneration::Read(appDir, socket, token) && HarborSessionGeneration::Valid(token));
    HarborSessionGeneration::Publisher competing;
    assert(!competing.Publish(appDir, path));
    auto other = socket;other.st_ino++;
    assert(HarborSessionGeneration::Read(appDir, other, token) && token.empty());
    assert(chmod(HarborSessionGeneration::Path(appDir).c_str(), 0644) == 0);
    assert(!HarborSessionGeneration::Read(appDir, socket, token));
    assert(chmod(HarborSessionGeneration::Path(appDir).c_str(), 0600) == 0);
  }
  // A crash can leave valid bytes, but it cannot retain the backend's lifetime lock.
  const auto record = HarborSessionGeneration::Path(appDir);
  int stale = open(record.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
  assert(stale >= 0);
  const std::string data = "pcl.harbor.session-generation.v1\t" + std::string(32, 'a') + "\t" + HarborSessionGeneration::SocketIdentity(socket) + "\n";
  assert(write(stale, data.data(), data.size()) == ssize_t(data.size()));close(stale);
  assert(HarborSessionGeneration::Read(appDir, socket, token) && token.empty());
  unlink(record.c_str());
  assert(symlink(path.c_str(), record.c_str()) == 0);
  assert(!HarborSessionGeneration::Read(appDir, socket, token));
  HarborSessionGeneration::Publisher unsafe;
  assert(!unsafe.Publish(appDir, path));
  unlink(record.c_str());close(fd);unlink(path.c_str());assert(rmdir(directory) == 0);
  std::cout << "session generation: live lock, exact socket, stale bytes, private permissions and symlink refusal\n";
}

static void
VersionTests()
{
  // Preserve exact-version semantics, even for a future build's own identical peer.
  for (const std::string version : { "0.1.21", "0.1.22", "0.1.23", "0.1.24", "0.1.25", "0.1.26", "0.1.27", "0.1.28", "0.1.29", "0.1.30", "0.1.31", "0.1.32" })
    assert(HarborSession::CompatibleVersions(version, version));
  assert(HarborSession::CompatibleVersions("0.1.23", "0.1.22"));
  assert(HarborSession::CompatibleVersions("0.1.24", "0.1.22"));
  assert(HarborSession::CompatibleVersions("0.1.24", "0.1.23"));
  assert(HarborSession::CompatibleVersions("0.1.25", "0.1.24"));
  assert(HarborSession::CompatibleVersions("0.1.26", "0.1.25"));
  assert(HarborSession::CompatibleVersions("0.1.27", "0.1.26"));
  assert(HarborSession::CompatibleVersions("0.1.28", "0.1.27"));
  assert(HarborSession::CompatibleVersions("0.1.29", "0.1.28"));
  assert(HarborSession::CompatibleVersions("0.1.30", "0.1.29"));
  assert(HarborSession::CompatibleVersions("0.1.31", "0.1.29"));
  assert(HarborSession::CompatibleVersions("0.1.31", "0.1.30"));
  assert(HarborSession::CompatibleVersions("0.1.32", "0.1.31"));
  for (const std::string client : { "0.1.21", "0.1.22", "0.1.23", "0.1.24", "0.1.25", "0.1.26", "0.1.27", "0.1.28", "0.1.29", "0.1.30", "0.1.31", "0.1.32", "1.0.0", "unknown", "" })
    for (const std::string backend : { "0.1.21", "0.1.22", "0.1.23", "0.1.24", "0.1.25", "0.1.26", "0.1.27", "0.1.28", "0.1.29", "0.1.30", "0.1.31", "0.1.32", "1.0.0", "unknown", "" })
    {
      const bool expected = client == backend || (client == "0.1.23" && backend == "0.1.22") ||
                            (client == "0.1.24" && (backend == "0.1.22" || backend == "0.1.23")) ||
                            (client == "0.1.25" && backend == "0.1.24") ||
                            (client == "0.1.26" && backend == "0.1.25") ||
                            (client == "0.1.27" && backend == "0.1.26") ||
                            (client == "0.1.28" && backend == "0.1.27") ||
                            (client == "0.1.29" && backend == "0.1.28") ||
                            (client == "0.1.30" && backend == "0.1.29") ||
                            (client == "0.1.31" && (backend == "0.1.29" || backend == "0.1.30")) ||
                            (client == "0.1.32" && backend == "0.1.31");
      assert(HarborSession::CompatibleVersions(client, backend) == expected);
    }
  assert(!HarborSession::CompatibleVersions("0.1.23", "0.1.22.1"));
  assert(!HarborSession::CompatibleVersions("0.1.23", "v0.1.22"));
  std::cout << "session versions: exact equality and only the verified directional 23/24/25/26/27/28/29/30/31/32 upgrade pairs\n";
}

int
main()
{
  CodecTests();
  ViewChatTests();
  SocketPathTests();
  GenerationTests();
  VersionTests();
  RoutingTests();
  UnknownSendTests(false);
  UnknownSendTests(true);
  std::cout << "session tests passed\n";
  return 0;
}
