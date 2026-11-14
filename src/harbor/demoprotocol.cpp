#include "demoprotocol.h"
#include "fileutil.h"
#include "protocolutil.h"
#include "timeutil.h"
#include <algorithm>
#include <cstdlib>

void
HarborDemo::Seed()
{
  const char* fixture = getenv("HARBOR_DEMO_FIXTURES");
  std::string dir = fixture ? fixture : FileUtil::DirName(FileUtil::GetSelfPath()) + "/../share/harbor/demo";
  auto add = [&](const std::string& chat,
                 const std::string& who,
                 const std::string& text,
                 int minute,
                 const std::string& file = "",
                 const std::string& mime = "")
  {
    ChatMessage m;
    m.id = std::to_string(m_NextId++);
    m.senderId = who;
    m.text = text;
    m.isOutgoing = who == "self";
    m.isRead = true;
    m.timeSent = 1794438000000LL + minute * 60000;
    if (!file.empty())
    {
      FileInfo f;
      f.fileId = file;
      f.filePath = dir + "/" + file;
      f.fileType = mime;
      f.fileStatus = FileStatusDownloaded;
      m.fileInfo = ProtocolUtil::FileInfoToHex(f);
    }
    m_Messages[chat].push_back(m);
  };
  add("marina", "marina", "Made it down to the water before sunrise.", 1);
  add("marina", "marina", "A little peace before the city wakes.", 2, "harbor.png", "image/png");
  add("marina", "self", "That light is unreal. Send me the view.", 3);
  add("marina", "marina", "", 4, "voice.wav", "audio/wav");
  add("marina", "marina", "The boats are just starting to move.", 5, "morning.mp4", "video/mp4");
  add("marina", "self", "Keeping this one for a slow morning.", 6);
  add("studio", "leo", "The new layout is ready to try.", 1);
  add("studio", "asha", "The conversation should feel calm even when the day isn't.", 2);
  add("studio", "self", "Keep the details quiet and the messages readable.", 3);
  for (int i = 0; i < 35; ++i)
    add("saved",
        "self",
        "Note " + std::to_string(i + 1) + ": Make the next action obvious. Leave more space between ideas.",
        i);
  if (getenv("HARBOR_DEMO_TOPICS"))
  {
    add("hive", "leo", "Parent group history.", 10);
    add("hive#421", "leo", "Topic-specific history for the Desk conversation.", 100);
    add("hive#550", "asha", "A different topic with separate history.", 90);
  }
  add("kleya", "kleya", "One conversation. A clear place to write. Everything else within reach.", 1);
}

bool
HarborDemo::Login()
{
  auto n = std::make_shared<ConnectNotify>(m_Id);
  n->success = true;
  Emit(n);
  if (const char* delay = getenv("HARBOR_DEMO_INCOMING_DELAY_MS"))
  {
    const int milliseconds = std::max(0, std::min(10000, atoi(delay)));
    m_LiveThread = std::thread(
      [this, milliseconds]
      {
        for (int waited = 0; waited < milliseconds && !m_Stopping; waited += 10)
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (m_Stopping)
          return;
        ChatMessage message;
        message.id = "live-incoming-90000";
        message.senderId = "marina";
        message.text = "A live incoming reply.";
        message.isOutgoing = false;
        message.timeSent = TimeUtil::GetCurrentTimeMSec();
        auto update = std::make_shared<NewMessagesNotify>(m_Id);
        update->success = true;
        update->chatId = "marina";
        update->chatMessages = { message };
        Emit(update);
      });
  }
  return true;
}
bool
HarborDemo::Logout()
{
  m_Stopping = true;
  if (m_LiveThread.joinable())
    m_LiveThread.join();
  return true;
}

void
HarborDemo::SendRequest(std::shared_ptr<RequestMessage> request)
{
  switch (request->GetMessageType())
  {
    case GetContactsRequestType:
    {
      auto n = std::make_shared<NewContactsNotify>(m_Id);
      for (const auto& item : std::vector<std::pair<std::string, std::string>>{ { "marina", "Marina" },
                                                                                { "studio", "Studio" },
                                                                                { "saved", "Saved Messages" },
                                                                                { "kleya", "Kleya" },
                                                                                { "leo", "Leo" },
                                                                                { "asha", "Asha" },
                                                                                { "self", "You" } })
      {
        ContactInfo c;
        c.id = item.first;
        c.name = item.second;
        c.isSelf = c.id == "self";
        n->contactInfos.push_back(c);
      }
      if (getenv("HARBOR_DEMO_TOPICS"))
      {
        for (auto pair : std::vector<std::pair<std::string, std::string>>{
               { "hive", "Team forum" },
               { "hive#421", "Team forum > Desk planning — DEMO-421" },
               { "hive#550", "Team forum > Infrastructure" } })
        {
          ContactInfo topic;
          topic.id = pair.first;
          topic.name = pair.second;
          n->contactInfos.push_back(topic);
        }
      }
      Emit(n);
      break;
    }
    case GetChatsRequestType:
    {
      auto n = std::make_shared<NewChatsNotify>(m_Id);
      n->success = true;
      for (const auto& item : m_Messages)
      {
        ChatInfo c;
        c.id = item.first;
        c.lastMessageTime = item.second.back().timeSent;
        c.actualLastMessageTime = item.second.back().timeSent;
        c.lastMessageText = item.second.back().text;
        if (getenv("HARBOR_DEMO_TOPICS") && c.id == "studio")
        {
          c.isPinned = true;
          c.lastMessageTime = 8000000000000000000LL;
        }
        c.isUnread = c.id == "marina";
        n->chatInfos.push_back(c);
      }
      Emit(n);
      break;
    }
    case GetMessagesRequestType:
    {
      auto r = std::static_pointer_cast<GetMessagesRequest>(request);
      auto n = std::make_shared<NewMessagesNotify>(m_Id);
      n->chatId = r->chatId;
      n->success = true;
      n->sequence = true;
      n->fromMsgId = r->fromMsgId;
      auto& messages = m_Messages[r->chatId];
      size_t end = messages.size();
      if (!r->fromMsgId.empty())
      {
        auto it = std::find_if(
          messages.begin(), messages.end(), [&](const ChatMessage& m) { return m.id == r->fromMsgId; });
        end = std::distance(messages.begin(), it);
      }
      size_t start = end > size_t(std::max(0, r->limit)) ? end - r->limit : 0;
      if (getenv("HARBOR_DEMO_PARTIAL_HISTORY") && r->fromMsgId.empty() && end > 0)
        start = end - 1;
      n->chatMessages.assign(messages.begin() + start, messages.begin() + end);
      Emit(n);
      break;
    }
    case SendMessageRequestType:
    {
      auto r = std::static_pointer_cast<SendMessageRequest>(request);
      if (getenv("HARBOR_DEMO_FAIL_SEND"))
      {
        auto failed = std::make_shared<SendMessageNotify>(m_Id);
        failed->success = false;
        failed->chatId = r->chatId;
        failed->chatMessage = r->chatMessage;
        Emit(failed);
        break;
      }
      ChatMessage m = r->chatMessage;
      m.id = std::to_string(m_NextId++);
      m.isOutgoing = true;
      m.senderId = "self";
      m.isRead = true;
      m.timeSent = TimeUtil::GetCurrentTimeMSec();
      m_Messages[r->chatId].push_back(m);
      auto n = std::make_shared<NewMessagesNotify>(m_Id);
      n->success = true;
      n->chatId = r->chatId;
      n->chatMessages = { m };
      Emit(n);
      auto sent = std::make_shared<SendMessageNotify>(m_Id);
      sent->success = true;
      sent->chatId = r->chatId;
      sent->chatMessage = m;
      Emit(sent);
      break;
    }
    case EditMessageRequestType:
    {
      auto r = std::static_pointer_cast<EditMessageRequest>(request);
      if (getenv("HARBOR_DEMO_FAIL_EDIT"))
      {
        auto failed = std::make_shared<SendMessageNotify>(m_Id);
        failed->success = false;
        failed->chatId = r->chatId;
        failed->chatMessage = r->chatMessage;
        failed->chatMessage.id = r->msgId;
        Emit(failed);
        break;
      }
      for (auto& m : m_Messages[r->chatId])
        if (m.id == r->msgId)
        {
          m.text = r->chatMessage.text;
          m.isEdited = true;
          auto n = std::make_shared<NewMessagesNotify>(m_Id);
          n->chatId = r->chatId;
          n->success = true;
          n->chatMessages = { m };
          Emit(n);
        }
      break;
    }
    case DeleteMessageRequestType:
    {
      auto r = std::static_pointer_cast<DeleteMessageRequest>(request);
      auto& list = m_Messages[r->chatId];
      list.erase(
        std::remove_if(list.begin(), list.end(), [&](const ChatMessage& m) { return m.id == r->msgId; }),
        list.end());
      auto n = std::make_shared<DeleteMessageNotify>(m_Id);
      n->success = true;
      n->chatId = r->chatId;
      n->msgId = r->msgId;
      Emit(n);
      break;
    }
    case DownloadFileRequestType:
    {
      auto r = std::static_pointer_cast<DownloadFileRequest>(request);
      for (const auto& m : m_Messages[r->chatId])
        if (m.id == r->msgId)
        {
          auto n = std::make_shared<NewMessageFileNotify>(m_Id);
          n->chatId = r->chatId;
          n->msgId = r->msgId;
          n->fileInfo = m.fileInfo;
          n->downloadFileAction = r->downloadFileAction;
          Emit(n);
        }
      break;
    }
    case SendReactionRequestType:
    {
      auto r = std::static_pointer_cast<SendReactionRequest>(request);
      auto n = std::make_shared<NewMessageReactionsNotify>(m_Id);
      n->chatId = r->chatId;
      n->msgId = r->msgId;
      n->reactions.emojiCounts[r->emoji] = 1;
      Emit(n);
      break;
    }
    default:
      break;
  }
}
