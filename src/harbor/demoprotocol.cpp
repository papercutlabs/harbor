#include "demoprotocol.h"
#include <iostream>
#include "fileutil.h"
#include "protocolutil.h"
#include "richtext.h"
#include "timeutil.h"
#include "strutil.h"
#include "forumid.h"
#include "launchroute.h"
#include "wire.h"
#include <algorithm>
#include <cstdlib>
#include <fstream>

namespace
{
// Offline route integration can use the same signed group IDs returned by CNS.
// The production path never reads these demo-only selectors.
int64_t
DemoRouteChat()
{
  const char* value = getenv("HARBOR_DEMO_ROUTE_CHAT_ID");
  HarborRoute::Target target{ value ? value : "12345", {}, {}, {} };
  HarborRoute::Validate(target);
  return TelegramForum::ChatNumber(target.conversation);
}
// The standalone Harbor fixture keeps its original IDs and stories. The fork's
// fixture uses the same shared route IDs as its desktop and Android test clients.
struct DuetMessage
{
  const char* sender;
  const char* text;
};
struct DuetChat
{
  int64_t roomId;
  int32_t topicId;
  const char* room;
  const char* title;
  int minutesAgo;
  std::vector<DuetMessage> messages;

  std::string Id() const { return topicId ? TelegramForum::ConversationId(roomId, topicId) : StrUtil::NumToHex(roomId); }
  std::string Name() const { return topicId ? std::string(room) + " > " + title : room; }
};
constexpr int64_t AgentsRoom = -100123450002LL;
std::vector<DuetChat>
DuetChats()
{
  const auto pcL = DemoRouteChat();
  return {
    { pcL, 0, "Papercut Labs", "", 55, {
      { "kleya", "Workspace trial is ready for a first pass. Use this group for anything that affects more than one team." },
      { "self", "I'll keep the focused questions in each topic." },
    } },
    { pcL, 550, "Papercut Labs", "Doug", 2, {
      { "doug", "Two things are ready for you. The Org Walk draft has one decision left, and the Prospaq team has a revised shortlist." },
      { "self", "Bring me the Org Walk decision first. I can read the shortlist afterward." },
      { "doug", "I'll keep the shortlist on hand. The question for the walk is who owns the return when a handoff crosses teams." },
    } },
    { pcL, 421, "Papercut Labs", "Org Walk", 9, {
      { "kleya", "I moved the handoff example next to the rule it illustrates. I used an internal handoff so the example can stand on its own." },
      { "self", "Give me the point where ownership changes." },
      { "kleya", "Ownership changes when the receiving lead accepts the outcome and its conditions in writing." },
    } },
    { pcL, 712, "Papercut Labs", "Keys & Accounts", 38, {
      { "keys", "The account inventory now shows which quota figures were refreshed and which are still from the previous read." },
      { "self", "Keep the older figures visible with their age. I need to distinguish a missing read from a low balance." },
      { "keys", "I’ll keep both values in separate columns." },
    } },
    { AgentsRoom, 0, "Papercut Agents", "", 720, {
      { "amelia", "The engagement notes have moved into their own topics. Keep the shared client questions here." },
    } },
    { AgentsRoom, 610, "Papercut Agents", "Prospaq", 17, {
      { "amelia", "The shortlist is down to six. Three matches have an operations lead who can sponsor a trial." },
      { "self", "Show me the three with a named sponsor." },
      { "amelia", "They’re grouped at the top with the problem each person is trying to solve." },
    } },
    { pcL, 1, "Papercut Labs", "General", 260, {
      { "doug", "Please leave the test workspaces as you found them after the review." },
      { "self", "I’ll keep the demo assignments isolated." },
    } },
    { pcL, 2, "Papercut Labs", "Lead A", 130, {
      { "leo", "The workspace checks are done. The newer build kept the draft during the switch." },
      { "self", "Attach the observation so the other device can be checked against it." },
    } },
    { pcL, 3, "Papercut Labs", "Lead B", 980, {
      { "asha", "The two panes can take different assignments now." },
      { "self", "Keep a report of what happens when both devices update the same pane." },
    } },
  };
}
// UTF-16 length, the unit native ranges use
int32_t
U16(const std::string& utf8)
{
  int32_t n = 0;
  for (char32_t c : RichText::Utf32(utf8))
    n += c > 0xFFFF ? 2 : 1;
  return n;
}
int
DelayMs(const char* name)
{
  const char* value = getenv(name);
  return value ? std::max(0, std::min(20000, atoi(value))) : 0;
}
}

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
  if (getenv("HARBOR_DEMO_PICTURES"))
  {
    m_Messages["studio"].clear();
    auto photo = [&](const std::string& id) {
      add("studio", "leo", id, id == "photo-shown" ? 80 : 20);
      auto& message = m_Messages["studio"].back();
      message.id = id;
      FileInfo file;
      file.fileId = dir + "/harbor.png";
      file.filePath = "[Photo]";
      file.fileType = std::string(getenv("HARBOR_DEMO_PICTURES")) == "legacy" ? "" : "image/jpeg";
      file.fileStatus = FileStatusNotDownloaded;
      message.fileInfo = ProtocolUtil::FileInfoToHex(file);
    };
    photo("photo-offscreen");
    for (int i = 0; i < 40; ++i) add("studio", "leo", "Earlier history " + std::to_string(i), 21 + i);
    photo("photo-shown");
  }
  if (getenv("HARBOR_DEMO_TOPICS"))
  {
    if (getenv("HARBOR_DEMO_ROUTES") && getenv("DUET_HARBOR_CATALOGUE"))
    {
      const auto now = TimeUtil::GetCurrentTimeMSec();
      for (const auto& chat : DuetChats())
      {
        const auto id = chat.Id();
        for (size_t i = 0; i < chat.messages.size(); ++i)
        {
          const auto& message = chat.messages[i];
          add(id, message.sender, message.text, 0);
          const auto age = chat.minutesAgo + int64_t(chat.messages.size() - i - 1) * 3;
          m_Messages[id].back().timeSent = now - age * 60000;
        }
      }
    }
    else
    {
      const bool routed = getenv("HARBOR_DEMO_ROUTES");
      const auto parent = routed ? StrUtil::NumToHex(DemoRouteChat()) : "hive";
      const auto topic = [&](int32_t id) { return routed ? TelegramForum::ConversationId(DemoRouteChat(), id) : parent + "#" + std::to_string(id); };
      add(parent, "leo", "Parent group history.", 10);
      add(topic(421), "leo", "Topic-specific history for the Desk conversation.", 100);
      add(topic(550), "asha", "A different topic with separate history.", 90);
    }
  }
  if (getenv("HARBOR_DEMO_LINKS")) {
    const auto topic = [&](int id) { return TelegramForum::ConversationId(DemoRouteChat(), id); };
    m_Messages[topic(421)].clear(); m_Messages[topic(550)].clear();
    add(topic(421), "leo", "Topic-specific history for the Desk conversation.", 100);
    m_Messages[topic(421)].back().id = StrUtil::NumToHex(int64_t(100) << 20);
    for (int index = 101; index <= 130; ++index) {
      add(topic(421), "leo", "Later message " + std::to_string(index), index);
      m_Messages[topic(421)].back().id = StrUtil::NumToHex(int64_t(index) << 20);
    }
    add(topic(550), "asha", "A different topic with separate history.", 90);
    m_Messages[topic(550)].back().id = StrUtil::NumToHex(int64_t(90) << 20);
    add(topic(550), "leo", "Open the linked Desk message.", 91);
    RichText::Message link; link.text = "Open the linked Desk message.";
    link.entities.push_back({RichText::TextUrl, 9, 19, "https://t.me/c/12345/421/100"});
    m_Messages[topic(550)].back().richText = RichText::Encode(link);
  }
  // Load a substantial offline conversation without synthesizing terminal sends.
  // Input-latency checks use the normal history response and native renderer.
  if (getenv("HARBOR_DEMO_LONG_HISTORY"))
  {
    std::string body;
    for (int i = 0; i < 80; ++i)
      body += "A long conversation keeps the full history while the reader selects text and writes a reply.\n\n";
    for (int i = 0; i < 50; ++i)
      add("studio", "leo", body + "selection-sentinel", 100 + i);
  }
  add("kleya", "kleya", "One conversation. A clear place to write. Everything else within reach.", 1);
  if (getenv("HARBOR_DEMO_RICH"))
    SeedRich(add);
}

// Native-formatted messages shaped like TDLib's formattedText: original text plus UTF-16 ranges.
void
HarborDemo::SeedRich(const std::function<void(const std::string&, const std::string&, const std::string&, int,
                                              const std::string&, const std::string&)>& add)
{
  auto rich = [&](const std::string& who, int minute, const std::string& text,
                  std::vector<RichText::Entity> entities, const std::string& quoted = "", bool pinned = false)
  {
    RichText::Message native;
    native.text = text;
    native.entities = entities;
    add("rich", who, text, minute, "", "");
    auto& m = m_Messages["rich"].back();
    m.richText = RichText::Encode(native);
    m.quotedId = quoted;
    m.quotedChatId = "rich";
    m.isPinned = pinned;
    return m.id;
  };
  if (getenv("HARBOR_DEMO_CLICKS"))
  {
    const std::string label = "PcL shelf rewrite draft with wrapped words";
    rich("doug", 1, "Read " + label + " now.",
         { { RichText::TextUrl, 5, U16(label), "https://example.com/shelf?view=full" } });
    rich("doug", 2, "Wide 你好 café é link end.",
         { { RichText::TextUrl, 5, U16("你好 café é link"), "https://example.com/wide" } });
    rich("doug", 3, "Hidden secret link.",
         { { RichText::TextUrl, 7, 11, "https://example.com/secret" }, { RichText::Spoiler, 7, 11, "" } });
    rich("doug", 4, "Unsafe local link.", { { RichText::TextUrl, 7, 10, "file:///tmp/do-not-open.sh" } });
    rich("doug", 5, "Normal underlined text.", { { RichText::Underline, 0, 22, "" } });
    rich("doug", 6, "Download this file.", {});
    FileInfo file;
    file.fileId = std::string(getenv("HARBOR_DEMO_FIXTURES")) + "/new-shelf.md";
    file.filePath = "/not-downloaded/new-shelf.md";
    file.fileType = "text/markdown";
    file.fileStatus = FileStatusNotDownloaded;
    m_Messages["rich"].back().fileInfo = ProtocolUtil::FileInfoToHex(file);
    return;
  }
  if (getenv("HARBOR_DEMO_CODE_COPY"))
  {
    const std::string shortCode = "printf 'right edge!'", shortPrefix = "Short command:\n";
    rich("doug", 1, shortPrefix + shortCode + "\nAfter short command.",
         { { RichText::Pre, U16(shortPrefix), U16(shortCode), "shell" } });
    const std::string prefix = "Multiline command:\n",
      command = "./a_very_long_script_name_that_requires_wrapping --keep-newlines\n\tprintf '你好 é'\nprintf 'tail'\n";
    rich("doug", 2, prefix + command + "After multiline command.",
         { { RichText::Pre, U16(prefix), U16(command), "bash" } });
    const std::string first = "echo first\n", second = "echo second", lead = "Two code blocks:\n", between = "Between blocks.\n";
    rich("doug", 3, lead + first + between + second,
         { { RichText::Pre, U16(lead), U16(first), "sh" },
           { RichText::Pre, U16(lead + first + between), U16(second), "" } });
    const std::string hidden = "printf 'secret'", hiddenPrefix = "Concealed code:\n";
    rich("doug", 4, hiddenPrefix + hidden,
         { { RichText::Pre, U16(hiddenPrefix), U16(hidden), "shell" },
           { RichText::Spoiler, U16(hiddenPrefix + "printf '"), 6, "" } });
    add("rich", "doug", "Legacy cached code:\n```shell\nprintf 'cached'\n\tprintf 'second'\n\n```\nAfter cached code.", 5, "", "");
    return;
  }
  if (getenv("HARBOR_DEMO_REPLIES"))
  {
    const std::string parentText = getenv("HARBOR_DEMO_LONG_REPLY")
      ? "Before we retire Diode, we need to be able to recover the approval records, even if an interrupted payment must be checked later. End of the complete original."
      : "Older parent outside the loaded page.";
    const auto parent = rich("doug", 1, parentText, {});
    for (int i = 0; i < 60; ++i)
      add("rich", "doug", "Older filler " + std::to_string(i), 2 + i, "", "");
    rich("doug", 70, "First received reply.", {}, parent);
    rich("doug", 71, "Second reply shares the parent.", {}, parent);
    // A same-id message exists in this conversation, but the reply belongs to another chat.
    rich("doug", 72, "Wrong same-chat collision.", {});
    auto collision = m_Messages["rich"].back();
    collision.senderId = "leo";
    collision.text = "Correct cross-chat original.";
    collision.richText = RichText::Encode({ collision.text, {} });
    m_Messages["studio"].push_back(collision);
    rich("doug", 73, "Cross-chat received reply.", {}, collision.id);
    m_Messages["rich"].back().quotedChatId = "studio";
    rich("doug", 74, "Legacy reply with lost chat metadata.", {}, collision.id);
    m_Messages["rich"].back().quotedChatId.clear();
    rich("doug", 75, "Unknown target with supplied quote.", {}, "0");
    auto& supplied = m_Messages["rich"].back();
    supplied.quotedChatId = "?";
    supplied.quotedSenderName = "Original author";
    supplied.quotedText = "secret words and public quote";
    supplied.quotedRichText = RichText::Encode({ supplied.quotedText, { { RichText::Spoiler, 0, 12, "" } } });
    rich("doug", 76, "Reply to inaccessible original.", {}, "missing");
    rich("doug", 77, "Reply to deleted original.", {}, "deleted");
    if (getenv("HARBOR_DEMO_LONG_REPLY"))
      rich("self", 78, "Outgoing wrapped reply.", {}, parent);
    if (getenv("HARBOR_DEMO_REPLY_CONTEXT"))
      rich("32363035373131333630", 79, "Unresolved sender keeps this source recognizable.", {});
    auto deleted = collision;
    deleted.id = "deleted";
    deleted.isDeleted = true;
    m_Messages["rich"].insert(m_Messages["rich"].begin(), deleted);
    return;
  }
  const auto first = rich("doug", 1, "Original plan: keep drafts unique and offline.",
                          { { RichText::Bold, 0, 13, "" } }, "", true);
  for (int i = 0; i < 60; ++i)
    add("rich", i % 2 ? "self" : "doug", "Filler " + std::to_string(i + 1) + " between the plan and today.", 2 + i,
        "", "");
  const std::string prefix = "It's in your Downloads now:\n\n", command = "sh ~/Downloads/create-hive-next-repos.sh",
                    suffix = "\n\nIt should end with Done. Paste me the output.";
  rich("doug", 70, prefix + command + suffix, { { RichText::Pre, U16(prefix), U16(command), "shell" } });
  const std::string intro = "Two steps 🙂:\n", one = "cd ~/src\n\tmake **all** _x_\n", mid = "then\n",
                    two = "printf '%s\\n' \"done\"";
  rich("doug", 71, intro + one + mid + two,
       { { RichText::Pre, U16(intro), U16(one), "" }, { RichText::Pre, U16(intro + one + mid), U16(two), "sh" } });
  const std::string links = "Read the plan and https://example.org/b then old news";
  rich("doug", 72, links,
       { { RichText::TextUrl, 9, 4, "https://example.com/plans/PCL-1312?view=full" },
         { RichText::Url, 18, 21, "" },
         { RichText::Strike, 45, 3, "" },
         { RichText::Underline, 0, 4, "" },
         { RichText::Italic, 49, 4, "" } }, "", true);
  const auto secret = rich("doug", 73, "The code word is harbor-42, keep it.", { { RichText::Spoiler, 17, 9, "" } });
  const std::string lead = "Quoted:\n", quote = "first quoted line\nsecond quoted line\nthird quoted line",
                    tail = "\nThat was the quote.";
  rich("doug", 74, lead + quote + tail, { { RichText::ExpandableQuote, U16(lead), U16(quote), "" } });
  rich("doug", 75, "Replying to the original plan.", {}, first);
  rich("doug", 76, "**literal** stars and `ticks` stay as typed.", {});
  rich("doug", 77, "This replies to a message that no longer exists.", {}, "999999");
  // a reply excerpt and the picker preview must keep these spoilers concealed; the long styled line wraps
  rich("doug", 78, "Got it, I will keep that word private.", {}, secret);
  const std::string wrapped = "Wrapped styling: every bold word here stays bold across the narrow edge.";
  rich("doug", 79, wrapped, { { RichText::Bold, 17, int32_t(U16(wrapped)) - 18, "" } });
  rich("doug", 80, "Last word: the deploy key is swordfish-7.", { { RichText::Spoiler, 29, 11, "" } });
}

bool
HarborDemo::Login()
{
  if (getenv("HARBOR_DEMO_AUTH"))
  {
    // Offline stand-in for TDLib's terminal sign-in: take control through the handler exactly as
    // TgChat does, prompt on stdout, read one line from stdin, release. Only the fixed nonsecret
    // demo code is accepted; the typed text itself is never recorded.
    auto take = std::make_shared<ProtocolUiControlNotify>(m_Id);
    take->isTakeControl = true;
    Emit(take);
    while (!take->isTakeControl && !take->cancelled && !m_Stopping)
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      take->isTakeControl = true;
      Emit(take);
    }
    if (take->cancelled || m_Stopping) return false;
    std::cout << "Enter authentication code: " << std::flush;
    std::string line;
    bool read;
    if (getenv("HARBOR_DEMO_AUTH_PASSWORD"))
    {
      line = StrUtil::GetPass(); // the existing native password terminal path
      read = !std::cin.fail() && !std::cin.eof();
    }
    else read = static_cast<bool>(std::getline(std::cin, line));
    Record(std::string("auth read=") + (read ? "1" : "0") + " accepted=" + (line == "12345" ? "1" : "0"));
    auto release = std::make_shared<ProtocolUiControlNotify>(m_Id);
    release->isTakeControl = false;
    Emit(release);
  }
  auto n = std::make_shared<ConnectNotify>(m_Id);
  n->success = true;
  Emit(n);
  // Offline typing events are driven by the PTY observer after its history/draft are ready.
  if (const char* control = getenv("HARBOR_DEMO_TYPING_CONTROL"))
  {
    const std::string chat = getenv("HARBOR_DEMO_TYPING_CHAT") ? getenv("HARBOR_DEMO_TYPING_CHAT") : "marina";
    // The PTY observer owns this private fixture file. Activity starts only after it has
    // loaded history and the draft; a real stop is requested after its active observations.
    m_Delayed.emplace_back([this, chat, path = std::string(control)] {
      size_t consumed = 0;
      bool expiring = false;
      std::chrono::steady_clock::time_point expiry;
      while (!m_Stopping)
      {
        std::ifstream input(path);
        std::string action;
        size_t index = 0;
        while (std::getline(input, action) && !m_Stopping)
        {
          if (++index <= consumed) continue;
          consumed = index;
          TypingEvent(chat, action);
          if (action == "expire" || (action == "renew" && expiring))
          {
            expiring = true;
            expiry = std::chrono::steady_clock::now() + std::chrono::milliseconds(5500);
          }
          else if (action != "renew") expiring = false;
          Record("typing-control " + std::to_string(index) + " " + action + " at-ms=" + std::to_string(TimeUtil::GetCurrentTimeMSec()));
        }
        if (expiring && std::chrono::steady_clock::now() >= expiry)
        {
          TypingEvent(chat, "stop");
          Record("typing-control expired at-ms=" + std::to_string(TimeUtil::GetCurrentTimeMSec()));
          expiring = false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
    });
  }
  if (const char* delay = getenv("HARBOR_DEMO_INCOMING_DELAY_MS"))
  {
    const int milliseconds = std::max(0, std::min(10000, atoi(delay)));
    m_LiveThread = std::thread(
      [this, milliseconds]
      {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
        while (std::chrono::steady_clock::now() < deadline && !m_Stopping)
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (m_Stopping)
          return;
        // HARBOR_DEMO_INCOMING_COUNT (1-9) delivers further messages 400 ms apart; each later update
        // also replays the previous message, as Telegram updates may, so duplicates are exercised.
        const char* countText = getenv("HARBOR_DEMO_INCOMING_COUNT");
        const int count = std::max(1, std::min(9, countText ? atoi(countText) : 1));
        // A burst may share one provider second. Match TDLib's message-id tie breaker as well.
        const int64_t incomingTime = TimeUtil::GetCurrentTimeMSec() / 1000 * 1000;
        ChatMessage previous;
        for (int i = 0; i < count && !m_Stopping; ++i)
        {
          if (i)
          {
            const auto pause = std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
            while (std::chrono::steady_clock::now() < pause && !m_Stopping)
              std::this_thread::sleep_for(std::chrono::milliseconds(10));
            if (m_Stopping)
              return;
          }
          ChatMessage message;
          message.id = "live-incoming-9000" + std::to_string(i);
          message.senderId = "marina";
          message.text = i ? "Another live message " + std::to_string(i + 1) + "." : "A live incoming reply.";
          message.isOutgoing = false;
          message.timeSent = incomingTime + (std::hash<std::string>{}(message.id) % 256);
          auto update = std::make_shared<NewMessagesNotify>(m_Id);
          update->success = true;
          update->chatId = "marina";
          update->chatMessages = { message };
          if (i)
            update->chatMessages.insert(update->chatMessages.begin(), previous);
          Emit(update);
          previous = message;
        }
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
  for (auto& thread : m_Delayed)
    if (thread.joinable())
      thread.join();
  m_Delayed.clear();
  return true;
}
// Demo-only controlled service events keep fixture lifetime separate from observer scheduling.
void
HarborDemo::TypingEvent(const std::string& chat, const std::string& action)
{
  if (action == "reply")
  {
    ChatMessage reply;
    reply.id = "typing-complete"; reply.senderId = "marina"; reply.isOutgoing = false;
    reply.text = "The active reply is complete."; reply.timeSent = TimeUtil::GetCurrentTimeMSec();
    auto incoming = std::make_shared<NewMessagesNotify>(m_Id);
    incoming->success = true; incoming->chatId = chat; incoming->chatMessages = { reply };
    Emit(incoming);
  }
  else if (action == "disconnect")
  {
    auto disconnected = std::make_shared<ConnectNotify>(m_Id);
    disconnected->success = false; Emit(disconnected);
  }
  else
  {
    auto typing = std::make_shared<ReceiveTypingNotify>(m_Id);
    typing->chatId = chat; typing->userId = "marina"; typing->isTyping = action != "stop";
    Emit(typing);
  }
}
// Offline request recorder: HARBOR_DEMO_REQUEST_LOG names a file that receives one line per request.
void
HarborDemo::Record(const std::string& line)
{
  if (const char* path = getenv("HARBOR_DEMO_REQUEST_LOG"))
  {
    std::lock_guard<std::mutex> lock(m_RecordMutex);
    std::ofstream(path, std::ios::app) << line << "\n";
  }
}
void
HarborDemo::Later(int milliseconds, std::shared_ptr<ServiceMessage> message)
{
  if (milliseconds <= 0)
  {
    Emit(message);
    return;
  }
  m_Delayed.emplace_back(
    [this, milliseconds, message]
    {
      const auto started = std::chrono::steady_clock::now();
      const auto deadline = started + std::chrono::milliseconds(milliseconds);
      int waited = 0;
      for (; std::chrono::steady_clock::now() < deadline && !m_Stopping; waited += 10)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      if (message->GetMessageType() == SearchMessagesNotifyType)
        Record(std::string(m_Stopping ? "dropped" : "emit") + " search token=" +
               std::to_string(std::static_pointer_cast<SearchMessagesNotify>(message)->token) +
               " nominal-ms=" + std::to_string(waited) + " elapsed-ms=" +
               std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::steady_clock::now() - started).count()));
      if (!m_Stopping)
      {
        if (message->GetMessageType() == SendMessageNotifyType)
        {
          auto n = std::static_pointer_cast<SendMessageNotify>(message);
          Record("emit send request=" + std::to_string(n->requestId) + " success=" + std::to_string(n->success));
        }
        Emit(message);
      }
    });
}

void
HarborDemo::SendRequest(std::shared_ptr<RequestMessage> request)
{
  switch (request->GetMessageType())
  {
    case GetContactsRequestType:
    {
      auto n = std::make_shared<NewContactsNotify>(m_Id);
      n->requestId = request->requestId;
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
        // HARBOR_DEMO_STUDIO_NAME supplies a provider-controlled name, e.g. one carrying terminal escapes.
        if (c.id == "studio" && getenv("HARBOR_DEMO_STUDIO_NAME"))
          c.name = getenv("HARBOR_DEMO_STUDIO_NAME");
        c.isSelf = c.id == "self";
        n->contactInfos.push_back(c);
      }
      if (getenv("HARBOR_DEMO_RICH"))
        for (auto pair : std::vector<std::pair<std::string, std::string>>{ { "rich", "Doug formatting" },
                                                                            { "doug", "Doug" } })
        {
          ContactInfo contact;
          contact.id = pair.first;
          contact.name = pair.second;
          n->contactInfos.push_back(contact);
        }
      if (getenv("HARBOR_DEMO_TOPICS"))
      {
        const bool routed = getenv("HARBOR_DEMO_ROUTES");
        if (routed && getenv("DUET_HARBOR_CATALOGUE"))
        {
          for (const auto& chat : DuetChats())
          {
            ContactInfo contact;
            contact.id = chat.Id();
            contact.name = chat.Name();
            n->contactInfos.push_back(contact);
          }
          for (const auto& sender : std::vector<std::pair<const char*, const char*>>{
                 { "doug", "Doug" }, { "amelia", "Amelia" }, { "keys", "Keys" } })
          {
            ContactInfo contact;
            contact.id = sender.first;
            contact.name = sender.second;
            n->contactInfos.push_back(contact);
          }
        }
        else
        {
          const auto parent = routed ? StrUtil::NumToHex(DemoRouteChat()) : "hive";
          for (const auto& entry : std::vector<std::pair<std::string, std::string>>{
                 { parent, routed ? "Routed group" : "Team forum" },
                 { routed ? TelegramForum::ConversationId(DemoRouteChat(), 421) : "hive#421", routed ? "Routed group > Agent" : "Team forum > Desk planning — DEMO-421" },
                 { routed ? TelegramForum::ConversationId(DemoRouteChat(), 550) : "hive#550", routed ? "Routed group > Doug" : "Team forum > Infrastructure" } })
          {
            ContactInfo topic;
            topic.id = entry.first;
            topic.name = entry.second;
            n->contactInfos.push_back(topic);
          }
        }
      }
      Emit(n);
      break;
    }
    case GetChatsRequestType:
    {
      auto n = std::make_shared<NewChatsNotify>(m_Id);
      n->success = true;
      n->requestId = request->requestId;
      for (const auto& item : m_Messages)
      {
        ChatInfo c;
        c.id = item.first;
        c.lastMessageTime = item.second.back().timeSent;
        c.actualLastMessageTime = item.second.back().timeSent;
        c.lastMessageText = RichText::Preview(item.second.back().richText, item.second.back().text);
        RichText::Message decoded;
        c.lastMessageNative = RichText::Decode(item.second.back().richText, decoded);
        if (getenv("HARBOR_DEMO_TOPICS") && c.id == "studio")
        {
          c.isPinned = true;
          c.lastMessageTime = 8000000000000000000LL;
        }
        const auto topic = TelegramForum::TopicNumber(c.id);
        c.isUnread = c.id == "marina" || (getenv("DUET_HARBOR_CATALOGUE") && (topic == 550 || topic == 712));
        n->chatInfos.push_back(c);
      }
      Emit(n);
      break;
    }
    case ResolveTelegramLinkRequestType:
    {
      auto r = std::static_pointer_cast<ResolveTelegramLinkRequest>(request);
      auto n = std::make_shared<ResolveTelegramLinkNotify>(m_Id); n->requestId = r->requestId;
      if (getenv("HARBOR_DEMO_ROUTES") && (r->username == "duet_demo" || r->url.find("t.me/duet_demo/") != std::string::npos)) {
        n->chat = std::to_string(DemoRouteChat()); n->success = true;
        if (r->username.empty()) { n->topic = "421"; n->message = "100"; }
      } else n->error = "Demo link unavailable";
      Emit(n); break;
    }
    case GetMessageRequestType:
    {
      auto r = std::static_pointer_cast<GetMessageRequest>(request);
      Record("get-message chat=" + r->chatId + " msg=" + r->msgId + (r->replyPreview ? " preview=1" : ""));
      auto& messages = m_Messages[r->chatId];
      auto it = std::find_if(messages.begin(), messages.end(), [&](const ChatMessage& m) { return m.id == r->msgId; });
      if (it == messages.end())
      {
        // unavailable originals get no reply, as with a deleted Telegram message, or with
        // HARBOR_DEMO_FETCH_ERROR the explicit failure TDLib reports for a message it cannot load
        if (r->replyPreview || getenv("HARBOR_DEMO_FETCH_ERROR"))
        {
          auto failure = std::make_shared<NewMessagesNotify>(m_Id);
          failure->chatId = r->chatId;
          failure->previewMsgId = r->replyPreview ? r->msgId : "";
          failure->success = false;
          failure->sequence = false;
          failure->requestId = r->requestId;
          Emit(failure);
        }
        break;
      }
      auto n = std::make_shared<NewMessagesNotify>(m_Id);
      n->chatId = r->chatId;
      n->success = true;
      n->sequence = false;
      n->chatMessages = { *it };
      n->previewMsgId = r->replyPreview ? r->msgId : "";
      n->requestId = r->requestId;
      Later(DelayMs("HARBOR_DEMO_FETCH_DELAY_MS"), n);
      break;
    }
    case SearchMessagesRequestType:
    {
      auto r = std::static_pointer_cast<SearchMessagesRequest>(request);
      Record("search chat=" + r->chatId + " query=" + r->query + " pinned=" + (r->pinned ? "1" : "0") +
             " token=" + std::to_string(r->token));
      auto n = std::make_shared<SearchMessagesNotify>(m_Id);
      n->chatId = r->chatId;
      n->query = r->query;
      n->pinned = r->pinned;
      n->token = r->token;
      n->requestId = r->requestId;
      if (getenv("HARBOR_DEMO_SEARCH_FAIL"))
        n->error = "demo search failure";
      else
      {
        n->success = true;
        const auto& messages = m_Messages[r->chatId];
        for (auto it = messages.rbegin(); it != messages.rend(); ++it)
          if (r->pinned ? it->isPinned : (!r->query.empty() && it->text.find(r->query) != std::string::npos))
            n->chatMessages.push_back(*it);
        n->totalCount = int32_t(n->chatMessages.size());
        if (int32_t(n->chatMessages.size()) > r->limit)
          n->chatMessages.resize(r->limit);
      }
      if (!getenv("HARBOR_DEMO_SEARCH_SILENT"))
        Later(DelayMs("HARBOR_DEMO_SEARCH_DELAY_MS"), n);
      break;
    }
    case GetAvailableReactionsRequestType:
    {
      auto r = std::static_pointer_cast<GetAvailableReactionsRequest>(request);
      Record("available-reactions chat=" + r->chatId + " msg=" + r->msgId);
      auto n = std::make_shared<AvailableReactionsNotify>(m_Id);
      n->chatId = r->chatId;
      n->msgId = r->msgId;
      n->requestId = r->requestId;
      const auto& messages = m_Messages[r->chatId];
      auto it = std::find_if(messages.begin(), messages.end(), [&](const ChatMessage& m) { return m.id == r->msgId; });
      if (it == messages.end() || getenv("HARBOR_DEMO_REACTIONS_FAIL"))
        n->success = false;
      else if (it->text.find("Filler") == 0)
        n->emojis = {}; // a message whose chat allows no reactions
      else if (it->text.find("quote") != std::string::npos)
        n->emojis = { "🎉", "🙏" }; // a narrower set than other messages, as a chat may allow
      else
        n->emojis = { "👍", "🔥", "🎉" };
      Later(DelayMs("HARBOR_DEMO_REACTIONS_DELAY_MS"), n);
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
      n->requestId = r->requestId;
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
      const int sendIndex = ++m_SendCount;
      HarborWire::Writer original;
      HarborWire::Put(original, r->chatMessage);
      Record("send chat=" + r->chatId + " text=" + r->chatMessage.text + " view=" +
             std::to_string(r->requestId >> 32) + " index=" + std::to_string(sendIndex) +
             " request=" + std::to_string(r->requestId) + " payload=" + StrUtil::StrToHex(original.buf));
      const int sendDelay = DelayMs("HARBOR_DEMO_SEND_DELAY_MS");
      // HARBOR_DEMO_FAIL_SEND fails every send; HARBOR_DEMO_FAIL_SEND_INDEX=n fails only the
      // n-th send this session receives, so two windows can send the same text with one failure.
      const int failIndex = DelayMs("HARBOR_DEMO_FAIL_SEND_INDEX");
      if (getenv("HARBOR_DEMO_FAIL_SEND") || (failIndex > 0 && failIndex == sendIndex))
      {
        auto failed = std::make_shared<SendMessageNotify>(m_Id);
        failed->success = false;
        failed->chatId = r->chatId;
        failed->chatMessage = r->chatMessage;
        failed->requestId = r->requestId;
        if (sendDelay > 0)
        {
          auto accepted = std::make_shared<SendMessageNotify>(*failed);
          accepted->success = true; accepted->pending = true;
          Emit(accepted);
        }
        Later(sendDelay, failed);
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
      if (getenv("HARBOR_DEMO_SEND_ID_REPLACEMENT"))
      {
        auto pending = std::make_shared<NewMessagesNotify>(*n);
        pending->chatMessages.front().id = "temporary-" + m.id;
        Emit(pending);
        auto removed = std::make_shared<DeleteMessageNotify>(m_Id);
        removed->success = true;
        removed->chatId = r->chatId;
        removed->msgId = pending->chatMessages.front().id;
        Emit(removed);
      }
      Emit(n);
      auto sent = std::make_shared<SendMessageNotify>(m_Id);
      sent->success = true;
      sent->chatId = r->chatId;
      sent->chatMessage = m;
      sent->requestId = r->requestId;
      if (sendDelay > 0)
      {
        auto accepted = std::make_shared<SendMessageNotify>(*sent);
        accepted->pending = true;
        Emit(accepted);
      }
      Later(sendDelay, sent);
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
        failed->requestId = r->requestId;
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
      n->requestId = r->requestId;
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
          if (getenv("HARBOR_DEMO_CLICKS") || getenv("HARBOR_DEMO_PICTURES"))
          {
            auto file = ProtocolUtil::FileInfoFromHex(m.fileInfo);
            file.filePath = file.fileId;
            file.fileStatus = FileStatusDownloaded;
            n->fileInfo = ProtocolUtil::FileInfoToHex(file);
            Record("attachment-download action=" + std::to_string(int(r->downloadFileAction)));
            if (getenv("HARBOR_DEMO_PICTURES")) Record("picture-download " + r->msgId + " action=" + std::to_string(int(r->downloadFileAction)));
          }
          n->downloadFileAction = r->downloadFileAction;
          n->requestId = r->requestId;
          Later(DelayMs("HARBOR_DEMO_PICTURE_DELAY_MS"), n);
        }
      break;
    }
    case SendReactionRequestType:
    {
      auto r = std::static_pointer_cast<SendReactionRequest>(request);
      Record("send-reaction chat=" + r->chatId + " msg=" + r->msgId + " emoji=" + r->emoji +
             " prev=" + r->prevEmoji);
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
