#include "harbor.h"
#include "apputil.h"
#include "clipboard.h"
#include "fileutil.h"
#include "format.h"
#include "media.h"
#include "messagecache.h"
#include "protocolutil.h"
#include "strutil.h"
#include "timeutil.h"
#include "ui.h"
#include "update.h"
#include <algorithm>
#include <clocale>
#include <cmath>
#include <cwctype>
#include <dirent.h>
#include <fstream>
#include <iostream>
#include <regex>
#include <set>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace
{
enum
{
  Base = 1,
  Muted,
  Accent,
  Received,
  Sent,
  Sender,
  Metadata,
  Selected,
  Surface,
  Good,
  Warning,
  SenderOut,
  MetadataOut,
  CodeIn,
  CodeOut,
  CodeSelected,
  LinkIn,
  LinkOut,
  LinkSelected,
  EmphasisSelected
};
enum
{
  FindKey = KEY_MAX + 101,
  NewlineKey,
  PasteStartKey,
  PasteEndKey,
  DeleteWordKey
};
std::wstring
W(const std::string& s)
{
  try
  {
    return StrUtil::ToWString(s);
  }
  catch (...)
  {
    return L"[invalid text]";
  }
}
std::string
S(const std::wstring& s)
{
  return StrUtil::ToString(s);
}
std::wstring
Safe(std::wstring s)
{
  for (auto& c : s)
    if ((c < 32 && c != '\n' && c != '\t') || c == 127)
      c = ' ';
  return s;
}
int
Width(wchar_t c)
{
  return std::max(0, wcwidth(c));
}
int
Width(const std::wstring& s)
{
  int n = 0;
  for (auto c : s)
    n += Width(c);
  return n;
}
struct TextLayout
{
  struct Point
  {
    int row = 0, col = 0;
  };
  std::vector<std::wstring> lines;
  std::vector<std::vector<size_t>> sources;
  std::vector<Point> cursors;
};
TextLayout
Layout(const std::wstring& source, int width)
{
  auto text = Safe(source);
  width = std::max(1, width);
  TextLayout out;
  out.cursors.resize(text.size() + 1);
  size_t start = 0;
  do
  {
    size_t end = start, space = std::wstring::npos;
    int cells = 0;
    while (end < text.size() && text[end] != L'\n')
    {
      int cw = text[end] == L'\t' ? 1 : Width(text[end]);
      if (cells + cw > width && end > start)
        break;
      if (text[end] == L' ' || text[end] == L'\t')
      {
        if (cells > 0)
          space = end;
      }
      cells += cw;
      ++end;
    }
    size_t next = end;
    if (end < text.size() && text[end] != L'\n' && space != std::wstring::npos)
    {
      end = space;
      next = space + 1;
      while (next < text.size() && (text[next] == L' ' || text[next] == L'\t'))
        ++next;
    }
    // A separator exactly at the boundary belongs to the soft break, not a new line.
    if (next < text.size() && (text[next] == L' ' || text[next] == L'\t'))
    {
      while (next < text.size() && (text[next] == L' ' || text[next] == L'\t'))
        ++next;
    }
    int row = int(out.lines.size()), col = 0;
    std::wstring line;
    std::vector<size_t> sources;
    for (size_t i = start; i < end; ++i)
    {
      out.cursors[i] = { row, col };
      wchar_t ch = text[i] == L'\t' ? L' ' : text[i];
      line += ch;
      sources.push_back(i);
      col += Width(ch);
    }
    out.lines.push_back(line);
    out.sources.push_back(std::move(sources));
    for (size_t i = end; i <= next; ++i)
      out.cursors[i] = { row, col };
    if (next == text.size())
      break;
    if (text[next] == L'\n')
      ++next;
    start = next;
  } while (start <= text.size());
  return out;
}
std::wstring
Pad(std::wstring value, int width)
{
  value = Safe(value);
  std::wstring out;
  int n = 0;
  for (auto c : value)
  {
    if (c == '\n')
      c = ' ';
    if (n + Width(c) > width)
      break;
    out += c;
    n += Width(c);
  }
  return out + std::wstring(std::max(0, width - n), ' ');
}
std::string
Time(int64_t value)
{
  if (value < 0)
    return "";
  time_t time = value / 1000;
  struct tm tm;
  localtime_r(&time, &tm);
  char text[32];
  strftime(text, sizeof(text), "%H:%M", &tm);
  return text;
}
// Nerd Fonts mdi-check / mdi-check-all are single integrated receipt glyphs.
// https://github.com/ryanoasis/nerd-fonts/blob/master/glyphnames.json
std::string
Receipt(bool read)
{
  const char* choice = getenv("HARBOR_ICONS");
  const char* program = getenv("TERM_PROGRAM");
  bool nerd = choice ? std::string(choice) == "nerd"
                     : getenv("CMUX_SURFACE_ID") || getenv("CMUX_WORKSPACE_ID") ||
                         (program && (std::string(program) == "ghostty" || std::string(program) == "cmux"));
  return read ? (nerd ? u8" \U000f012d" : " read") : (nerd ? u8" \U000f012c" : " ✓");
}
bool
File(const std::string& p)
{
  struct stat st;
  return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}
bool
Directory(const std::string& p)
{
  struct stat st;
  return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}
std::string
Expand(std::string p)
{
  if (p == "~" || p.find("~/") == 0)
  {
    const char* h = getenv("HOME");
    p = std::string(h ? h : "") + p.substr(1);
  }
  return p;
}
std::string
Type(const std::string& path)
{
  std::string ext = FileUtil::BaseName(path);
  std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
  if (std::regex_search(ext, std::regex("\\.(png|jpe?g|webp|gif|heic)$")))
    return "image/*";
  if (std::regex_search(ext, std::regex("\\.(mp4|mov|mkv|webm)$")))
    return "video/*";
  if (std::regex_search(ext, std::regex("\\.(ogg|opus|mp3|wav|m4a|flac)$")))
    return "audio/*";
  return "application/octet-stream";
}
std::string
Size(const std::string& p)
{
  struct stat st;
  if (stat(p.c_str(), &st))
    return "unavailable";
  return FileUtil::GetSuffixedSize(st.st_size);
}
}

Harbor::Harbor()
  : m_Media(new HarborMedia)
{
  m_Directory = FileUtil::GetCurrentWorkingDir();
  m_Motion = getenv("HARBOR_REDUCED_MOTION") == nullptr;
  LoadState();
}
Harbor::~Harbor() = default;
std::string
Harbor::Key(const std::string& p, const std::string& c) const
{
  return p + "\t" + c;
}
Harbor::Conversation*
Harbor::Current()
{
  auto it = m_Chats.find(m_Current);
  return it == m_Chats.end() ? nullptr : &it->second;
}
Harbor::Conversation&
Harbor::Ensure(const std::string& p, const std::string& id)
{
  auto& c = m_Chats[Key(p, id)];
  if (c.profile.empty())
  {
    auto saved = m_SavedStates.find(Key(p, id));
    if (saved != m_SavedStates.end())
      c = saved->second;
  }
  c.profile = p;
  c.id = id;
  c.name = Name(p, id);
  return c;
}
std::string
Harbor::Name(const std::string& p, const std::string& id) const
{
  auto a = m_Contacts.find(p);
  if (a != m_Contacts.end())
  {
    auto b = a->second.find(id);
    if (b != a->second.end() && !b->second.name.empty())
      return b->second.name;
  }
  return id;
}
void
Harbor::AddProtocol(std::shared_ptr<Protocol> p)
{
  m_Protocols[p->GetProfileId()] = p;
}
std::unordered_map<std::string, std::shared_ptr<Protocol>>
Harbor::Protocols()
{
  return m_Protocols;
}
void
Harbor::Receive(std::shared_ptr<ServiceMessage> e)
{
  std::lock_guard<std::mutex> lock(m_Mutex);
  m_Events.push_back(e);
}
void
Harbor::Request(const std::string& p, std::shared_ptr<RequestMessage> r)
{
  auto it = m_Protocols.find(p);
  if (it != m_Protocols.end())
    it->second->SendRequest(r);
}
void
Harbor::Notify(const std::string& s)
{
  m_Status = s;
  m_StatusUntil = TimeUtil::GetCurrentTimeMSec() + 5000;
  m_Dirty = true;
}

void
Harbor::Init()
{
  setlocale(LC_ALL, "");
  initscr();
  raw();
  noecho();
  nonl();
  keypad(stdscr, TRUE);
  timeout(20);
  set_escdelay(60);
  curs_set(1);
  start_color();
  use_default_colors();
  const std::vector<std::tuple<short, int, int, int>> palette = {
    { 16, 20, 27, 37 },    { 17, 220, 231, 243 }, { 18, 161, 178, 201 },
    { 19, 159, 202, 244 }, { 20, 29, 43, 60 },    { 21, 41, 59, 70 },
    { 22, 185, 213, 184 }, { 23, 52, 71, 95 },    { 24, 224, 193, 152 }
  };
  if (can_change_color() && COLORS >= 256)
    for (auto item : palette)
      init_color(std::get<0>(item),
                 std::get<1>(item) * 1000 / 255,
                 std::get<2>(item) * 1000 / 255,
                 std::get<3>(item) * 1000 / 255);
  auto pair = [&](short id, short fg, short bg)
  {
    if (COLORS >= 256)
      init_pair(id, fg, bg);
    else
      init_pair(id, fg == 19 ? COLOR_CYAN : COLOR_WHITE, -1);
  };
  pair(Base, 17, 16);
  pair(Muted, 18, 16);
  pair(Accent, 19, 16);
  pair(Received, 17, 20);
  pair(Sent, 17, 21);
  pair(Sender, 19, 20);
  pair(Metadata, 18, 20);
  pair(Selected, 17, 23);
  pair(Surface, 18, 20);
  pair(Good, 22, 16);
  pair(Warning, 24, 16);
  pair(SenderOut, 22, 21);
  pair(MetadataOut, 18, 21);
  pair(CodeIn, 22, 20);
  pair(CodeOut, 22, 21);
  pair(CodeSelected, 22, 23);
  pair(LinkIn, 19, 20);
  pair(LinkOut, 19, 21);
  pair(LinkSelected, 19, 23);
  pair(EmphasisSelected, 19, 23);
  define_key("\033k", FindKey);
  define_key("\033[27;3;107~", FindKey);
  define_key("\033[107;3u", FindKey);
  // Ghostty/cmux use CSI-u even for ordinary control chords once enhanced keys are enabled.
  for (int code = 1; code <= 26; code++)
  {
    const std::string sequence = "\033[" + std::to_string('a' + code - 1) + ";5u";
    define_key(sequence.c_str(), KEY_MAX + 300 + code);
  }
  define_key("\033[13u", KEY_MAX + 313);
  define_key("\033[27u", KEY_MAX + 327);
  define_key("\033[127u", KEY_BACKSPACE);
  define_key("\033[127;2u", KEY_BACKSPACE);
  define_key("\033\177", DeleteWordKey);
  define_key("\033\b", DeleteWordKey);
  define_key("\033[127;3u", DeleteWordKey);
  define_key("\033[8;3u", DeleteWordKey);
  define_key("\033[27;3;127~", DeleteWordKey);
  define_key("\033[27;3;8~", DeleteWordKey);
  define_key("\033[13;2u", NewlineKey);
  // Legacy terminals encode modified Return as Escape followed by Return.
  // Decode it atomically: separate Esc/Return events browse and open message actions.
  define_key("\033\r", NewlineKey);
  define_key("\033[27;2;13~", NewlineKey);
  define_key("\033[13;2~", NewlineKey);
  define_key("\033[200~", PasteStartKey);
  define_key("\033[201~", PasteEndKey);
  mousemask(ALL_MOUSE_EVENTS | REPORT_MOUSE_POSITION, nullptr);
  mouseinterval(0);
  printf("\033[?1003l\033[?1002h\033[?2004h\033[>1u");
  fflush(stdout);
  m_Ready = true;
  for (auto& p : m_Protocols)
  {
    MessageCache::FetchContacts(p.first);
  }
}
void
Harbor::Cleanup()
{
  if (!m_Ready)
    return;
  SaveState();
  m_Media->Stop();
  m_Media->ClearGraphics();
  printf("\033[?1002l\033[<u\033[?2004l");
  mousemask(0, nullptr);
  curs_set(1);
  endwin();
  fflush(stdout);
  m_Ready = false;
}
void
Harbor::Run()
{
  int64_t lastSave = 0;
  while (m_Running && !AppUtil::IsTerminateRequested())
  {
    if (!m_MouseDown)
      ProcessEvents();
    if (m_AuthSuspended)
    {
      TimeUtil::Sleep(.02);
      continue;
    }
    const auto now = TimeUtil::GetCurrentTimeMSec();
    // A terminal can lose button-up when the pointer leaves its window.
    if (m_MouseDown && now - m_MouseActivityAt > 30000)
    {
      m_MouseDown = m_MouseDragging = m_HasSelection = false;
      m_Dirty = true;
    }
    if (now - m_LastUpdateCheck > 3000)
    {
      CheckUpdate();
      m_LastUpdateCheck = now;
    }
    if (auto* c = Current())
      if (c->historyRetryAt && c->historyStalls < 3 && now >= c->historyRetryAt)
        m_Dirty = true;
    if (now - lastSave > 2000)
    {
      SaveState();
      lastSave = now;
    }
    if (m_StatusUntil && now > m_StatusUntil)
    {
      m_StatusUntil = 0;
      m_Status.clear();
      m_Dirty = true;
    }
    if (m_Media->Poll())
      m_Dirty = true;
    if (m_Pasting && now - m_PasteStart > 10000)
    {
      m_Pasting = false;
      Paste(m_Paste);
      m_Paste.clear();
      Notify("Paste captured; review before sending");
    }
    if (m_AnimatedUntil)
    {
      m_Dirty = true;
      if (now >= m_AnimatedUntil)
        m_AnimatedUntil = 0;
    }
    if (m_Dirty && now - m_LastDraw >= 16)
    {
      Draw();
      m_LastDraw = now;
      m_Dirty = false;
    }
    wint_t k;
    int result = get_wch(&k);
    if (result != ERR)
    {
      if (m_Pasting)
      {
        if (k == PasteEndKey)
        {
          m_Pasting = false;
          Paste(m_Paste);
          m_Paste.clear();
        }
        else if (result != KEY_CODE_YES && m_Paste.size() < 4 * 1024 * 1024)
          m_Paste += wchar_t(k);
      }
      else
      {
        m_KeyIsSpecial = result == KEY_CODE_YES;
        if (m_KeyIsSpecial && k > KEY_MAX + 300 && k <= KEY_MAX + 327)
        {
          k -= KEY_MAX + 300;
          m_KeyIsSpecial = false;
        }
        KeyPress(k);
      }
    }
  }
  for (auto& p : m_Protocols)
  {
    auto r = std::make_shared<SetStatusRequest>();
    r->isOnline = false;
    Request(p.first, r);
  }
}
void
Harbor::LoadState()
{
  std::ifstream input(FileUtil::GetApplicationDir() + "/harbor-state");
  std::string line;
  size_t total = 0;
  while (std::getline(input, line) && total < 8 * 1024 * 1024)
  {
    total += line.size();
    std::istringstream row(line);
    std::string key, draft, selected, reply, editing, backup, backupReply;
    size_t backupCursor = 0;
    size_t cursor = 0;
    int scroll = 0, follow = 1;
    if (!(row >> key >> draft >> cursor >> scroll >> follow >> selected))
      continue;
    try
    {
      Conversation c;
      c.draft = draft == "-" ? L"" : W(StrUtil::StrFromHex(draft));
      c.cursor = std::min(cursor, c.draft.size());
      c.scroll = std::max(0, scroll);
      c.follow = follow != 0;
      c.selected = selected == "-" ? "" : StrUtil::StrFromHex(selected);
      if (row >> reply >> editing)
      {
        c.reply = reply == "-" ? "" : StrUtil::StrFromHex(reply);
        c.editing = editing == "-" ? "" : StrUtil::StrFromHex(editing);
      }
      if (row >> backup >> backupCursor >> backupReply)
      {
        c.draftBeforeEdit = backup == "-" ? L"" : W(StrUtil::StrFromHex(backup));
        c.cursorBeforeEdit = std::min(backupCursor, c.draftBeforeEdit.size());
        c.replyBeforeEdit = backupReply == "-" ? "" : StrUtil::StrFromHex(backupReply);
      }
      m_SavedStates[StrUtil::StrFromHex(key)] = c;
    }
    catch (...)
    {
    }
  }
}
void
Harbor::SaveState()
{
  std::map<std::string, Conversation> states = m_SavedStates;
  for (const auto& c : m_Chats)
  {
    auto& saved = states[c.first];
    saved.draft = c.second.draft;
    saved.cursor = c.second.cursor;
    saved.scroll = c.second.scroll;
    saved.follow = c.second.follow;
    saved.selected = c.second.selected;
    saved.reply = c.second.reply;
    saved.editing = c.second.editing;
    saved.draftBeforeEdit = c.second.draftBeforeEdit;
    saved.cursorBeforeEdit = c.second.cursorBeforeEdit;
    saved.replyBeforeEdit = c.second.replyBeforeEdit;
  }
  std::ostringstream out;
  for (const auto& item : states)
  {
    const auto& c = item.second;
    out << StrUtil::StrToHex(item.first) << ' ' << (c.draft.empty() ? "-" : StrUtil::StrToHex(S(c.draft)))
        << ' ' << c.cursor << ' ' << c.scroll << ' ' << c.follow << ' '
        << (c.selected.empty() ? "-" : StrUtil::StrToHex(c.selected)) << ' '
        << (c.reply.empty() ? "-" : StrUtil::StrToHex(c.reply)) << ' '
        << (c.editing.empty() ? "-" : StrUtil::StrToHex(c.editing)) << ' '
        << (c.draftBeforeEdit.empty() ? "-" : StrUtil::StrToHex(S(c.draftBeforeEdit))) << ' '
        << c.cursorBeforeEdit << ' '
        << (c.replyBeforeEdit.empty() ? "-" : StrUtil::StrToHex(c.replyBeforeEdit)) << '\n';
  }
  if (out.str() == m_LastState)
    return;
  const auto path = FileUtil::GetApplicationDir() + "/harbor-state";
  std::ofstream file(path + ".tmp", std::ios::trunc);
  file << out.str();
  file.close();
  if (file && rename((path + ".tmp").c_str(), path.c_str()) == 0)
  {
    chmod(path.c_str(), 0600);
    m_LastState = out.str();
  }
}
void
Harbor::ProcessEvents()
{
  std::deque<std::shared_ptr<ServiceMessage>> events;
  {
    std::lock_guard<std::mutex> lock(m_Mutex);
    events.swap(m_Events);
  }
  for (auto& e : events)
    Event(e);
}
void
Harbor::SortChats()
{
  m_ChatOrder.clear();
  for (auto& p : m_Chats)
    m_ChatOrder.push_back(p.first);
  std::sort(m_ChatOrder.begin(),
            m_ChatOrder.end(),
            [&](const std::string& a, const std::string& b)
            {
              return m_Chats[a].last != m_Chats[b].last ? m_Chats[a].last > m_Chats[b].last
                                                        : m_Chats[a].name < m_Chats[b].name;
            });
}
void
Harbor::Merge(Conversation& c, const std::vector<ChatMessage>& messages)
{
  const int64_t previousLatest = c.order.empty() ? -1 : c.messages[c.order.back()].timeSent;
  if (m_Current == Key(c.profile, c.id) && c.loaded && !c.follow && !m_Browse)
  {
    BuildRows();
    if (c.scroll + std::max(1, m_Bottom - m_Top) >= int(m_Rows.size()) - 2)
      c.follow = true;
  }
  for (const auto& m : messages)
  {
    if (!c.follow && !m.isOutgoing && m.timeSent > previousLatest && !c.messages.count(m.id))
      c.unseen++;
    c.messages[m.id] = m;
    c.last = std::max(c.last, m.timeSent);
  }
  c.order.clear();
  for (auto& m : c.messages)
    if (!m.second.isDeleted)
      c.order.push_back(m.first);
  std::sort(c.order.begin(),
            c.order.end(),
            [&](const std::string& a, const std::string& b)
            {
              return c.messages[a].timeSent != c.messages[b].timeSent
                       ? c.messages[a].timeSent < c.messages[b].timeSent
                       : a < b;
            });
  SortChats();
}
void
Harbor::Event(std::shared_ptr<ServiceMessage> e)
{
  const auto& p = e->profileId;
  switch (e->GetMessageType())
  {
    case ConnectNotifyType:
    {
      auto n = std::static_pointer_cast<ConnectNotify>(e);
      if (n->success)
      {
        m_Connected.insert(p);
        Request(p, std::make_shared<GetContactsRequest>());
        Request(p, std::make_shared<GetChatsRequest>());
        auto r = std::make_shared<SetStatusRequest>();
        r->isOnline = true;
        Request(p, r);
        Notify("Connected");
      }
      else
        Notify("Connection failed; retry with harbor -s");
      break;
    }
    case NewContactsNotifyType:
    {
      auto n = std::static_pointer_cast<NewContactsNotify>(e);
      for (const auto& c : n->contactInfos)
        m_Contacts[p][c.id] = c;
      for (auto& c : m_Chats)
        c.second.name = Name(c.second.profile, c.second.id);
      SortChats();
      break;
    }
    case NewChatsNotifyType:
    {
      auto n = std::static_pointer_cast<NewChatsNotify>(e);
      for (const auto& info : n->chatInfos)
      {
        auto& c = Ensure(p, info.id);
        c.last = std::max(c.last, info.lastMessageTime);
        c.unread = info.isUnread;
        if (c.loaded && m_Current == Key(p, info.id) && !c.order.empty() &&
            info.lastMessageTime > c.messages[c.order.back()].timeSent)
          Fetch(c);
      }
      SortChats();
      if (m_Current.empty() && !m_ChatOrder.empty())
        OpenChat(m_ChatOrder.front());
      break;
    }
    case NewMessagesNotifyType:
    {
      auto n = std::static_pointer_cast<NewMessagesNotify>(e);
      auto& c = Ensure(p, n->chatId);
      const auto oldest = c.order.empty() ? std::string() : c.order.front();
      int previousRows = 0;
      const bool older = n->sequence && !n->fromMsgId.empty();
      if (older && !c.follow && m_Current == Key(p, c.id))
      {
        BuildRows();
        previousRows = int(m_Rows.size());
      }
      Merge(c, n->chatMessages);
      if (n->success)
        c.loaded = true;
      if (n->sequence)
      {
        c.fetching = false;
        if (n->success && older && n->chatMessages.empty())
          c.more = false;
        if (older && !n->chatMessages.empty() && !c.order.empty() && c.order.front() == oldest)
        {
          ++c.historyStalls;
          c.historyRetryAt = TimeUtil::GetCurrentTimeMSec() + 250 * c.historyStalls;
        }
        else
        {
          c.historyStalls = 0;
          c.historyRetryAt = 0;
        }
      }
      if (previousRows && !c.follow)
      {
        BuildRows();
        c.scroll += std::max(0, int(m_Rows.size()) - previousRows);
      }
      break;
    }
    case SendMessageNotifyType:
    {
      auto n = std::static_pointer_cast<SendMessageNotify>(e);
      if (n->success)
      {
        if (!n->chatMessage.id.empty())
          Merge(Ensure(p, n->chatId), { n->chatMessage });
        Notify("Sent");
      }
      else
      {
        auto& c = Ensure(p, n->chatId);
        c.recovery.push_back(n->chatMessage);
        if (c.draft.empty())
        {
          const bool edit = !n->chatMessage.id.empty() && c.messages.count(n->chatMessage.id);
          if (edit)
            BeginEdit(c, n->chatMessage.id);
          c.draft = W(n->chatMessage.text);
          c.cursor = c.draft.size();
          if (!edit)
            c.reply = n->chatMessage.quotedId;
        }
        Notify("Send failed · draft restored; / → Recover");
      }
      break;
    }
    case DeleteMessageNotifyType:
    {
      auto n = std::static_pointer_cast<DeleteMessageNotify>(e);
      if (n->success)
      {
        auto& c = Ensure(p, n->chatId);
        c.messages.erase(n->msgId);
        Merge(c, {});
      }
      else
        Notify("Could not delete the message");
      break;
    }
    case NewMessageFileNotifyType:
    {
      auto n = std::static_pointer_cast<NewMessageFileNotify>(e);
      auto& c = Ensure(p, n->chatId);
      auto it = c.messages.find(n->msgId);
      if (it != c.messages.end())
      {
        it->second.fileInfo = n->fileInfo;
        if (n->downloadFileAction == DownloadFileActionOpen)
          OpenAttachment(p, n->chatId, n->msgId);
      }
      break;
    }
    case NewMessageReactionsNotifyType:
    {
      auto n = std::static_pointer_cast<NewMessageReactionsNotify>(e);
      auto& c = Ensure(p, n->chatId);
      auto it = c.messages.find(n->msgId);
      if (it != c.messages.end())
        it->second.reactions = n->reactions;
      break;
    }
    case NewMessageStatusNotifyType:
    {
      auto n = std::static_pointer_cast<NewMessageStatusNotify>(e);
      auto& c = Ensure(p, n->chatId);
      auto it = c.messages.find(n->msgId);
      if (it != c.messages.end())
        it->second.isRead = n->isRead;
      break;
    }
    case ReceiveTypingNotifyType:
    {
      auto n = std::static_pointer_cast<ReceiveTypingNotify>(e);
      if (n->isTyping && m_Current == Key(p, n->chatId))
        Notify(Name(p, n->userId) + " is typing…");
      break;
    }
    case ProtocolUiControlNotifyType:
    {
      auto n = std::static_pointer_cast<ProtocolUiControlNotify>(e);
      if (n->isTakeControl)
      {
        if (!m_AuthOwner.empty() && m_AuthOwner != p)
        {
          n->isTakeControl = false;
          break;
        }
        m_AuthOwner = p;
        endwin();
        m_AuthSuspended = true;
      }
      else if (m_AuthOwner == p)
      {
        m_AuthOwner.clear();
        m_AuthSuspended = false;
        refresh();
      }
      break;
    }
    case RequestAppExitNotifyType:
      m_Running = false;
      break;
    default:
      break;
  }
  if (m_Menu == "chats" || m_Menu == "forward")
    RebuildPicker();
  m_Dirty = true;
}
void
Harbor::Fetch(Conversation& c, bool older, bool automatic)
{
  if (c.fetching || (!c.more && older))
    return;
  if (!automatic)
    c.historyStalls = 0;
  c.historyRetryAt = 0;
  auto r = std::make_shared<GetMessagesRequest>();
  r->chatId = c.id;
  r->limit = 50;
  if (older && !c.order.empty())
    r->fromMsgId = c.order.front();
  c.fetching = true;
  Request(c.profile, r);
}
void
Harbor::OpenChat(const std::string& key)
{
  auto it = m_Chats.find(key);
  if (it == m_Chats.end())
    return;
  m_MouseDown = m_MouseDragging = m_HasSelection = false;
  m_Current = key;
  auto& c = it->second;
  auto r = std::make_shared<SetCurrentChatRequest>();
  r->chatId = c.id;
  Request(c.profile, r);
  Fetch(c);
  if (!c.editing.empty() && !c.messages.count(c.editing))
  {
    auto request = std::make_shared<GetMessageRequest>();
    request->chatId = c.id;
    request->msgId = c.editing;
    Request(c.profile, request);
  }
  m_Browse = false;
  m_Media->Stop();
  m_Dirty = true;
}
void
Harbor::Text(int y, int x, int width, const std::wstring& s, int color, bool bold)
{
  if (y < 0 || y >= m_H || x < 0 || x >= m_W || width < 1)
    return;
  attrset(COLOR_PAIR(color) | (bold ? A_BOLD : 0));
  auto value = Pad(s, std::min(width, m_W - x));
  mvaddnwstr(y, x, value.c_str(), value.size());
}
void
Harbor::Fill(int y, int x, int width, int color)
{
  Text(y, x, width, L"", color);
}

void
Harbor::BuildRows()
{
  m_Rows.clear();
  m_CopyTexts.clear();
  auto* c = Current();
  if (!c)
    return;
  int width = std::max(8, m_MainW - 10);
  std::string previousDay;
  for (const auto& id : c->order)
  {
    const auto& m = c->messages[id];
    time_t date = m.timeSent / 1000;
    struct tm calendar;
    localtime_r(&date, &calendar);
    char day[64];
    strftime(day, sizeof(day), "%a · %d %b", &calendar);
    if (previousDay != day)
    {
      m_Rows.push_back({ W(day), "", false, false });
      m_Rows.push_back({ L"", "", false, false });
      previousDay = day;
    }
    m_Rows.push_back({ L"", id, m.isOutgoing, false });
    const auto name = m.isOutgoing ? "You" : Name(c->profile, m.senderId);
    std::string time =
      Time(m.timeSent) + (m.isOutgoing ? Receipt(m.isRead) : "") + (m.isEdited ? " · edited" : "");
    auto left = W(name), right = W(time);
    int room = std::max(1, width - Width(right) - 1);
    m_Rows.push_back({ Pad(left, room) + L" " + right, id, m.isOutgoing, true });
    if (!m.quotedId.empty())
      m_Rows.push_back(
        { L"│ " + W(m.quotedText.empty() ? "Reply to a message" : m.quotedText), id, m.isOutgoing, false });
    if (!m.text.empty())
    {
      auto formatted = HarborFormatting::Parse(Safe(W(m.text)));
      m_CopyTexts[id] = formatted.text;
      auto layout = Layout(formatted.text, width);
      std::map<std::pair<int, int>, unsigned> styles;
      for (size_t i = 0; i < formatted.text.size(); ++i)
        if (!iswspace(formatted.text[i]))
          styles[{ layout.cursors[i].row, layout.cursors[i].col }] |= formatted.styles[i];
      for (size_t r = 0; r < layout.lines.size(); ++r)
      {
        Row row{ layout.lines[r], id, m.isOutgoing, false };
        row.sources = layout.sources[r];
        int col = 0;
        for (auto ch : row.text)
        {
          row.styles.push_back(styles[{ int(r), col }]);
          col += Width(ch);
        }
        m_Rows.push_back(std::move(row));
      }
    }
    if (!m.fileInfo.empty())
    {
      FileInfo f = ProtocolUtil::FileInfoFromHex(m.fileInfo);
      std::string type = f.fileType.empty() ? Type(f.filePath) : f.fileType;
      std::string symbol = type.find("image") == 0   ? "▧ "
                           : type.find("video") == 0 ? "▷ "
                           : type.find("audio") == 0 ? "▷ Voice · "
                                                     : "↳ ";
      auto label = symbol + FileUtil::BaseName(f.filePath);
      if (f.fileStatus == FileStatusDownloading)
        label += " · downloading";
      else if (f.fileStatus != FileStatusDownloaded)
        label += " · Enter to download";
      m_Rows.push_back({ W(label), id, m.isOutgoing, false });
      if (File(f.filePath) && (type.find("image") == 0 || type.find("video") == 0))
      {
        int imageRows = 7;

        for (int i = 0; i < imageRows; i++)
          m_Rows.push_back({ L"\1" + W(f.filePath), id, m.isOutgoing, false });
      }
      if (type.find("audio") == 0)
      {
        const bool active = m_Media->PlayingPath() == f.filePath && m_Media->Playing();
        m_Rows.push_back({ W(active ? (m_Media->Paused() ? "Ⅱ paused · " : "▶ playing · ") +
                                        std::to_string(int(m_Media->Position())) + "s"
                                    : "▁▃▆▄▂▅▇▃▁▅▄▂ · Enter for playback"),
                           id,
                           m.isOutgoing,
                           false });
      }
    }
    if (!m.reactions.emojiCounts.empty())
    {
      std::string value;
      for (auto& r : m.reactions.emojiCounts)
        value += r.first + " " + std::to_string(r.second) + " ";
      m_Rows.push_back({ W(value), id, m.isOutgoing, false });
    }
    m_Rows.push_back({ L"", id, m.isOutgoing, false });
    m_Rows.push_back({ L"", "", false, false });
  }
}

void
Harbor::Draw()
{
  m_Media->BeginFrame();
  getmaxyx(stdscr, m_H, m_W);
  if (m_W < 24 || m_H < 12)
  {
    erase();
    mvaddstr(0, 0, "Harbor · enlarge terminal");
    refresh();
    m_Media->EndFrame();
    return;
  }
  m_MainX = m_W >= 100 ? 25 : 0;
  m_MainW = m_W - m_MainX;
  auto* c = Current();
  int textWidth = std::max(1, m_MainW - 11);
  auto draftLayout = Layout(c ? c->draft : L"", textWidth);
  auto& draftLines = draftLayout.lines;
  m_ComposerRows = std::min(std::max(2, std::min(5, m_H / 4)), std::max(2, int(draftLines.size())));
  m_ComposerY = m_H - 4 - m_ComposerRows;
  m_Bottom = m_ComposerY - 1;
  if (c && (!c->reply.empty() || !c->editing.empty()))
    --m_Bottom;
  m_Top = 5;
  bkgd(COLOR_PAIR(Base));
  erase();
  m_Hits.clear();
  m_CopyCells.clear();
  Text(0, 0, m_W, L" HARBOR / TELEGRAM", Surface);
  Text(0,
       std::max(0, m_W - 17),
       17,
       !m_AuthOwner.empty()  ? L" ● authenticating"
       : m_Connected.empty() ? L" ○ connecting"
                             : L" ● connected",
       Surface);
  Text(2, m_MainX + 2, m_MainW - 20, c ? W(c->name) : L"Connecting…", Accent, true);
  Text(2, m_W - 16, 14, L"⌥K chats", Muted);
  m_Hits.push_back({ 2, m_W - 17, 17, "chats", "" });
  Text(3, m_MainX + 2, m_MainW - 4, c ? W(m_Protocols[c->profile]->GetProfileDisplayName()) : L"", Muted);
  if (m_MainX)
  {
    for (int y = 1; y < m_H; y++)
      Text(y, 24, 1, L"│", Muted);
    Text(2, 2, 20, L"CONVERSATIONS", Muted);
    int y = 4;
    for (auto& key : m_ChatOrder)
    {
      if (y >= m_H - 2)
        break;
      auto& chat = m_Chats[key];
      Text(y,
           1,
           22,
           W((chat.unread ? "• " : "  ") + chat.name),
           key == m_Current ? Selected : Muted,
           key == m_Current);
      m_Hits.push_back({ y, 1, 22, "chat", key });
      y += 2;
    }
  }
  BuildRows();
  if (c)
  {
    int visible = std::max(1, m_Bottom - m_Top);
    // A TDLib history response can be shorter than its requested limit. Fill
    // the viewport instead of waiting for the first manual scroll to paginate.
    if (c->loaded && c->more && !c->fetching && !c->order.empty() && c->historyStalls < 3 &&
        TimeUtil::GetCurrentTimeMSec() >= c->historyRetryAt && int(m_Rows.size()) < visible)
      Fetch(*c, true, true);
    if (c->follow)
    {
      c->unseen = 0;
      c->scroll = std::max(0, int(m_Rows.size()) - visible);
    }
    c->scroll = std::max(0, std::min(c->scroll, std::max(0, int(m_Rows.size()) - visible)));
    struct Picture
    {
      const HarborImage* image;
      int y, x, cols, rows, sourceRow;
    };
    std::vector<Picture> pictures;
    std::map<std::string, int> imageStarts;
    const int historyY = m_Top + std::max(0, visible - int(m_Rows.size()));
    for (int i = c->scroll, y = historyY; i < int(m_Rows.size()) && y < m_Bottom; i++, y++)
    {
      const auto& row = m_Rows[i];
      if (!row.id.empty())
      {
        auto& message = c->messages[row.id];
        if (!message.fileInfo.empty())
        {
          auto file = ProtocolUtil::FileInfoFromHex(message.fileInfo);
          if (file.fileStatus == FileStatusNotDownloaded &&
              (file.fileType.find("image") == 0 || file.fileType.find("video") == 0))
          {
            auto request = std::make_shared<DownloadFileRequest>();
            request->chatId = c->id;
            request->msgId = message.id;
            request->fileId = file.fileId;
            file.fileStatus = FileStatusDownloading;
            message.fileInfo = ProtocolUtil::FileInfoToHex(file);
            Request(c->profile, request);
          }
        }
      }
      int x = m_MainX + (row.sent ? 4 : 2);
      int w = m_MainW - 6;
      const bool selected = m_Browse && row.id == c->selected && !row.id.empty();
      const int color = selected ? Selected : row.sent ? Sent : Received;
      if (row.id.empty())
      {
        Text(y, m_MainX + 2, m_MainW - 4, row.text, Muted);
        continue;
      }
      Fill(y, x, w, color);
      Text(y,
           x + 2,
           w - 4,
           (row.header || (row.text.size() && row.text[0] == 1)) ? L"" : row.text,
           color,
           row.header);
      if (!row.styles.empty())
      {
        int col = 0;
        for (size_t j = 0; j < row.text.size(); ++j)
        {
          using namespace HarborFormatting;
          unsigned style = row.styles[j];
          if (!style)
          {
            col += Width(row.text[j]);
            continue;
          }
          int pair = color;
          int attr = 0;
          if (style & Quote)
            pair = selected ? Selected : row.sent ? MetadataOut : Metadata;
          if (style & Emphasis)
          {
            pair = selected ? EmphasisSelected : row.sent ? LinkOut : LinkIn;
            attr |= A_BOLD;
          }
          if (style & Code)
            pair = selected ? CodeSelected : row.sent ? CodeOut : CodeIn;
          if (style & Link)
          {
            pair = selected ? LinkSelected : row.sent ? LinkOut : LinkIn;
            attr |= A_UNDERLINE;
          }
          if (style & Underline)
            attr |= A_UNDERLINE;
          if (style & Strike)
          {
            pair = selected ? Selected : row.sent ? MetadataOut : Metadata;
            attr |= A_DIM;
          }
#ifdef A_ITALIC
          if (style & Italic)
            attr |= A_ITALIC;
#else
          if (style & Italic)
            attr |= A_UNDERLINE;
#endif
          attrset(COLOR_PAIR(pair) | attr);
          mvaddnwstr(y, x + 2 + col, &row.text[j], 1);
          col += Width(row.text[j]);
        }
      }
      if (row.header)
      {
        const auto& message = c->messages[row.id];
        std::wstring name = W(message.isOutgoing ? "You" : Name(c->profile, message.senderId));
        auto time = W(Time(message.timeSent) + (message.isOutgoing ? Receipt(message.isRead) : "") +
                      (message.isEdited ? " · edited" : ""));
        const int tw = std::min((w - 4) / 2, Width(time));
        Text(y,
             x + 2,
             std::max(1, w - tw - 5),
             name,
             selected   ? Selected
             : row.sent ? SenderOut
                        : Sender,
             true);
        Text(y, x + w - tw - 2, tw, time, selected ? Selected : row.sent ? MetadataOut : Metadata);
      }
      if (!row.sources.empty())
      {
        int col = 0;
        for (size_t j = 0; j < row.text.size(); ++j)
        {
          int width = Width(row.text[j]);
          if (!width)
            continue;
          size_t end = j + 1;
          while (end < row.text.size() && !Width(row.text[end]))
            ++end;
          CopyCell cell{ y, x + 2 + col, width, row.id, row.sources[j], row.sources[end - 1] + 1 };
          m_CopyCells.push_back(cell);
          if (CellSelected(cell))
          {
            attrset(COLOR_PAIR(Selected) | A_BOLD);
            mvaddnwstr(y, cell.x, row.text.c_str() + j, end - j);
          }
          col += width;
        }
      }
      m_Hits.push_back({ y, x, w, "message", row.id });
      if (row.text.size() && row.text[0] == 1)
      {
        const auto path = S(row.text.substr(1));
        int count = imageStarts[row.id]++;
        if (count == 0)
        {
          int sourceRow = 0;
          for (int j = i - 1;
               j >= 0 && m_Rows[j].id == row.id && !m_Rows[j].text.empty() && m_Rows[j].text[0] == 1;
               --j)
            ++sourceRow;
          int cols = std::min(m_MainW - 10, 56);
          const auto file = ProtocolUtil::FileInfoFromHex(c->messages[row.id].fileInfo);
          m_Media->RequestImage(path, cols, 7, file.fileType.find("video") == 0);
          const auto* image = m_Media->Image(path, cols, 7);
          if (image)
            pictures.push_back(
              { image, y, x + 2, std::min(cols, w - 4), std::min(7 - sourceRow, m_Bottom - y), sourceRow });
          else
            Text(y, x, w, L" Loading preview…", Muted);
        }
      }
    }
    if (m_Menu.empty())
      for (const auto& picture : pictures)
        m_Media->DrawImage(
          *picture.image, picture.y, picture.x, picture.cols, picture.rows, picture.sourceRow);
    if (!c->reply.empty() || !c->editing.empty())
      Text(m_ComposerY - 1,
           m_MainX + 2,
           m_MainW - 4,
           W(!c->editing.empty() ? "Editing your message · Esc cancels" : "Replying · " + c->reply),
           Accent);
    for (int row = 0; row < m_ComposerRows + 2; ++row)
      Fill(m_ComposerY + row, m_MainX + 2, m_MainW - 4, Surface);
    auto point = draftLayout.cursors[std::min(c->cursor, c->draft.size())];
    m_ComposerFirstLine = std::max(0, std::min(m_ComposerFirstLine, int(draftLines.size()) - m_ComposerRows));
    if (point.row < m_ComposerFirstLine)
      m_ComposerFirstLine = point.row;
    if (point.row >= m_ComposerFirstLine + m_ComposerRows)
      m_ComposerFirstLine = point.row - m_ComposerRows + 1;
    Text(m_ComposerY + 1, m_MainX + 4, 2, L"›", Surface);
    for (int i = 0; i < m_ComposerRows && m_ComposerFirstLine + i < int(draftLines.size()); i++)
      Text(m_ComposerY + 1 + i, m_MainX + 6, textWidth, draftLines[m_ComposerFirstLine + i], Surface);
  }
  else
    Text(6, m_MainX + 2, m_MainW - 4, L"Waiting for your conversations…", Muted);
  Text(
    m_H - 2, m_MainX + 2, std::max(1, m_MainW - 17), m_Browse ? L"MESSAGES · ↑↓ · Enter" : L"COMPOSE", Muted);
  {
    Text(m_H - 2, m_W - 13, 11, L"/ commands", Muted);
    m_Hits.push_back({ m_H - 2, m_W - 13, 11, "commands", "" });
  }
  Text(m_H - 1, m_MainX + 2, m_MainW - 4, W(m_Status), m_Status.empty() ? Muted : Good);
  if (!m_UpdateTarget.empty())
  {
    Text(m_H - 1, m_MainX + 2, m_MainW - 4, W("↑ Harbor " + m_UpdateVersion + " ready · / → Update"), Accent);
    m_Hits.push_back({ m_H - 1, m_MainX + 2, m_MainW - 4, "update", "" });
  }
  if (c && (!c->follow) &&
      (c->unseen > 0 || c->scroll + std::max(1, m_Bottom - m_Top) < int(m_Rows.size()) - 2))
  {
    const std::string label =
      c->unseen > 0 ? "↓ " + std::to_string(c->unseen) + " new · Ctrl+G" : "↓ Latest · Ctrl+G";
    Text(m_H - 2, m_MainX + 2, std::max(1, m_MainW - 17), W(label), Accent);
    m_Hits.push_back({ m_H - 2, m_MainX + 2, std::max(1, m_MainW - 17), "latest", "" });
  }
  if (!m_Menu.empty())
    DrawMenu();
  if (c && m_Menu.empty() && !m_Browse)
  {
    auto point = draftLayout.cursors[std::min(c->cursor, c->draft.size())];
    int cy = point.row - m_ComposerFirstLine;
    if (cy >= 0 && cy < m_ComposerRows)
    {
      move(m_ComposerY + 1 + cy, std::min(m_W - 3, m_MainX + 6 + point.col));
      curs_set(1);
    }
    else
      curs_set(0);
  }
  else if (m_Menu.empty())
    curs_set(0);
  refresh();
  m_Media->EndFrame();
}

void
Harbor::Latest()
{
  auto* c = Current();
  if (!c)
    return;
  c->follow = true;
  c->unseen = 0;
  c->selected.clear();
  m_Browse = false;
  m_Dirty = true;
}

void
Harbor::Select(int delta)
{
  auto* c = Current();
  if (!c || c->order.empty())
    return;
  int index = int(c->order.size()) - 1;
  auto it = std::find(c->order.begin(), c->order.end(), c->selected);
  if (it != c->order.end())
    index = int(it - c->order.begin()) + delta;
  index = std::max(0, std::min(index, int(c->order.size()) - 1));
  c->selected = c->order[index];
  m_Browse = true;
  c->follow = false;
  BuildRows();
  int first = -1, last = 0;
  for (int i = 0; i < int(m_Rows.size()); i++)
    if (m_Rows[i].id == c->selected)
    {
      if (first < 0)
        first = i;
      last = i;
    }
  int visible = std::max(1, m_Bottom - m_Top);
  if (first < c->scroll)
    c->scroll = first;
  else if (last >= c->scroll + visible)
    c->scroll = std::max(first, last - visible + 1);
  m_Dirty = true;
}
void
Harbor::Insert(const std::wstring& text)
{
  auto* c = Current();
  if (!c)
    return;
  c->cursor = std::min(c->cursor, c->draft.size());
  c->follow = true;
  c->unseen = 0;
  c->draft.insert(c->cursor, Safe(text));
  c->cursor += text.size();
  m_Browse = false;
  m_Dirty = true;
}
void
Harbor::EditKey(wint_t k)
{
  auto* c = Current();
  if (!c)
    return;
  c->cursor = std::min(c->cursor, c->draft.size());
  if (k == KEY_LEFT)
  {
    if (c->cursor)
      c->cursor--;
    while (c->cursor && Width(c->draft[c->cursor]) == 0)
      c->cursor--;
  }
  else if (k == KEY_RIGHT)
  {
    if (c->cursor < c->draft.size())
      c->cursor++;
    while (c->cursor < c->draft.size() && Width(c->draft[c->cursor]) == 0)
      c->cursor++;
  }
  else if (k == KEY_HOME || k == 1)
    c->cursor = 0;
  else if (k == KEY_END || k == 5)
    c->cursor = c->draft.size();
  else if (k == DeleteWordKey || k == 23)
  {
    size_t start = c->cursor;
    while (start && iswspace(c->draft[start - 1]))
      --start;
    while (start && !iswspace(c->draft[start - 1]))
      --start;
    c->draft.erase(start, c->cursor - start);
    c->cursor = start;
    m_Browse = false;
  }
  else if (k == KEY_BACKSPACE || k == 127 || k == 8)
  {
    if (c->cursor)
    {
      size_t start = c->cursor - 1;
      while (start && Width(c->draft[start]) == 0)
        start--;
      c->draft.erase(start, c->cursor - start);
      c->cursor = start;
    }
  }
  else if (k == KEY_DC)
  {
    if (c->cursor < c->draft.size())
    {
      size_t end = c->cursor + 1;
      while (end < c->draft.size() && Width(c->draft[end]) == 0)
        end++;
      c->draft.erase(c->cursor, end - c->cursor);
    }
  }
  else if (k == 21)
  {
    c->draft.erase(0, c->cursor);
    c->cursor = 0;
  }
  else if (k == KEY_UP || k == KEY_DOWN)
  {
    auto layout = Layout(c->draft, std::max(1, m_MainW - 11));
    auto point = layout.cursors[c->cursor];
    int target = point.row + (k == KEY_UP ? -1 : 1);
    size_t best = c->cursor;
    int distance = 1000000;
    for (size_t i = 0; i < layout.cursors.size(); ++i)
    {
      auto candidate = layout.cursors[i];
      if (candidate.row == target && std::abs(candidate.col - point.col) < distance)
      {
        best = i;
        distance = std::abs(candidate.col - point.col);
      }
    }
    c->cursor = best;
  }
  else if (k == NewlineKey || k == 10)
  {
    Insert(L"\n");
  }
  else if (k >= 32 && k < KEY_MIN)
  {
    Insert(std::wstring(1, wchar_t(k)));
  }
  else if (k > KEY_MAX && k <= 0x10ffff && k != FindKey && k != NewlineKey && k != PasteStartKey &&
           k != PasteEndKey)
  {
    Insert(std::wstring(1, wchar_t(k)));
  }
  m_Dirty = true;
}
void
Harbor::BeginEdit(Conversation& c, const std::string& id)
{
  if (c.editing.empty())
  {
    c.draftBeforeEdit = c.draft;
    c.cursorBeforeEdit = c.cursor;
    c.replyBeforeEdit = c.reply;
  }
  c.editing = id;
  c.reply.clear();
  auto original = c.messages.find(id);
  if (original != c.messages.end())
    c.draft = W(original->second.text);
  c.cursor = c.draft.size();
}
void
Harbor::EndEdit(Conversation& c)
{
  c.editing.clear();
  c.draft = std::move(c.draftBeforeEdit);
  c.cursor = std::min(c.cursorBeforeEdit, c.draft.size());
  c.reply = std::move(c.replyBeforeEdit);
  c.draftBeforeEdit.clear();
  c.replyBeforeEdit.clear();
  c.cursorBeforeEdit = 0;
}
void
Harbor::Send()
{
  auto* c = Current();
  if (!c || c->draft.empty())
    return;
  if (!c->editing.empty() && !c->messages.count(c->editing))
  {
    Notify("Loading the original message · Esc cancels editing");
    return;
  }
  auto paths = HarborPaths(c->draft);
  if (!paths.empty())
  {
    AttachPreview(paths);
    return;
  }
  ChatMessage m;
  if (!c->editing.empty())
  {
    auto original = c->messages.find(c->editing);
    if (original != c->messages.end())
      m = original->second;
  }
  m.text = S(c->draft);
  m.quotedId = c->reply;
  if (!c->reply.empty())
  {
    auto it = c->messages.find(c->reply);
    if (it != c->messages.end())
    {
      m.quotedText = it->second.text;
      m.quotedSender = it->second.senderId;
    }
  }
  const bool wasEditing = !c->editing.empty();
  if (wasEditing)
  {
    auto r = std::make_shared<EditMessageRequest>();
    r->chatId = c->id;
    r->msgId = c->editing;
    r->chatMessage = m;
    Request(c->profile, r);
    Notify("Edit requested");
  }
  else
  {
    auto r = std::make_shared<SendMessageRequest>();
    r->chatId = c->id;
    r->chatMessage = m;
    Request(c->profile, r);
    Notify("Sending…");
  }
  if (wasEditing)
    EndEdit(*c);
  else
  {
    c->draft.clear();
    c->cursor = 0;
    c->reply.clear();
  }
  c->follow = true;
  m_Browse = false;
  m_Dirty = true;
}
void
Harbor::Paste(const std::wstring& value)
{
  auto paths = HarborPaths(value);
  if (!paths.empty())
    AttachPreview(paths);
  else
    Insert(value);
}
void
Harbor::CloseMenu()
{
  m_Menu.clear();
  m_Filter.clear();
  m_Options.clear();
  m_Option = 0;
  m_MenuOffset = 0;
  m_Dirty = true;
}
void
Harbor::Choose(int index)
{
  if (index < 0 || index >= int(m_Options.size()))
    return;
  auto run = m_Options[index].run;
  run();
  m_Dirty = true;
}
void
Harbor::ChatPicker(bool forward)
{
  m_Menu = forward ? "forward" : "chats";
  if (m_Motion)
    m_AnimatedUntil = TimeUtil::GetCurrentTimeMSec() + 80;
  m_Filter.clear();
  m_Option = 0;
  m_MenuOffset = 0;
  RebuildPicker();
  m_Dirty = true;
}
void
Harbor::RebuildPicker()
{
  m_Options.clear();
  for (auto key : m_ChatOrder)
  {
    auto& c = m_Chats[key];
    auto folded = StrUtil::ToFold(c.name);
    if (!m_Filter.empty() && folded.find(StrUtil::ToFold(S(m_Filter))) == std::string::npos)
      continue;
    const bool forward = m_Menu == "forward";
    m_Options.push_back({ c.name,
                          c.unread ? "unread" : "",
                          [this, key, forward]
                          {
                            if (forward)
                            {
                              auto message = m_ForwardMessage;
                              std::string source = Name(m_ForwardProfile, message.senderId);
                              CloseMenu();
                              OpenChat(key);
                              auto* target = Current();
                              if (target)
                              {
                                std::wstring draft = L"Forwarded from " + W(source) + L"\n" + W(message.text);
                                if (!message.fileInfo.empty())
                                {
                                  auto f = ProtocolUtil::FileInfoFromHex(message.fileInfo);
                                  if (!File(f.filePath))
                                  {
                                    Notify("Download the attachment before forwarding");
                                    return;
                                  }
                                  m_Attachments = { f.filePath };
                                  target->draft = draft;
                                  target->cursor = draft.size();
                                  AttachPreview(m_Attachments);
                                }
                                else
                                {
                                  if (!target->draft.empty())
                                    target->draft += L"\n";
                                  target->draft += draft;
                                  target->cursor = target->draft.size();
                                  Notify("Forward prepared · Enter to send");
                                }
                              }
                            }
                            else
                            {
                              CloseMenu();
                              OpenChat(key);
                            }
                          } });
  }
  m_Option = std::max(0, std::min(m_Option, int(m_Options.size()) - 1));
}
void
Harbor::CheckUpdate()
{
  const char* home = getenv("HOME");
  std::string launcher = std::string(home ? home : "") + "/.local/bin/harbor";
  if (getenv("HARBOR_DEMO_FIXTURES") && getenv("HARBOR_TEST_UPDATE_LAUNCHER"))
    launcher = getenv("HARBOR_TEST_UPDATE_LAUNCHER");
  auto previous = m_UpdateTarget;
  m_UpdateTarget = HarborUpdate::Candidate(FileUtil::GetSelfPath(), launcher, m_UpdateVersion);
  if (previous != m_UpdateTarget)
    m_Dirty = true;
}
void
Harbor::UpdatePrompt()
{
  CheckUpdate();
  if (m_UpdateTarget.empty())
  {
    CloseMenu();
    Notify("You're running the latest installed Harbor");
    return;
  }
  m_Menu = "update";
  m_Option = 0;
  m_MenuOffset = 0;
  m_Options = { { "Restart into Harbor " + m_UpdateVersion,
                  "Your draft and reading position are saved",
                  [this]
                  {
                    CheckUpdate();
                    if (m_UpdateTarget.empty())
                    {
                      CloseMenu();
                      Notify("Update is no longer available");
                      return;
                    }
                    SaveState();
                    HarborUpdate::RestartTarget() = m_UpdateTarget;
                    m_Running = false;
                  } },
                { "Later", "Keep using this version", [this] { CloseMenu(); } } };
  m_Dirty = true;
}
void
Harbor::Commands()
{
  m_Menu = "commands";
  m_Filter.clear();
  m_Option = 0;
  m_MenuOffset = 0;
  m_Options = {
    { "More message tools", "For the selected message", [this] { Actions(false); } },
    { "Find a conversation", "⌥K / Ctrl+K", [this] { ChatPicker(); } },
    { "Jump to latest messages",
      "Ctrl+G",
      [this]
      {
        CloseMenu();
        Latest();
      } },
    { "Refresh conversation",
      "Ctrl+R",
      [this]
      {
        CloseMenu();
        auto* c = Current();
        if (c)
        {
          c->fetching = false;
          Fetch(*c);
          Notify("Refreshing conversation…");
        }
      } },
    { "Browse messages",
      "Esc · arrows · Enter",
      [this]
      {
        CloseMenu();
        Select(0);
      } },
    { "Attach files", "Ctrl+T / paste paths", [this] { FilePicker(m_Directory); } },
    { "Paste clipboard",
      "Ctrl+V · images preview",
      [this]
      {
        CloseMenu();
        ClipboardPaste();
      } },
    { "Pause / resume voice",
      "Space in playback",
      [this]
      {
        CloseMenu();
        m_Media->TogglePause();
      } },
    { "Stop playback",
      "",
      [this]
      {
        CloseMenu();
        m_Media->Stop();
      } },
    { "Keyboard help",
      "Enter sends · Shift+Enter newline",
      [this]
      {
        m_Menu = "help";
        m_Options = {
          { "Compose immediately", "Type from messages to write", [this] { CloseMenu(); } },
          { "Enter sends / Shift+Enter newline", "Ctrl+J is a newline fallback", [this] { CloseMenu(); } },
          { "Escape switches focus", "Arrows browse · Enter actions", [this] { CloseMenu(); } },
          { "Option+K finds conversations",
            "Map Option+K to Esc k; Ctrl+K fallback",
            [this] { CloseMenu(); } },
          { "Paste / drop previews attachments",
            "Ctrl+V reads clipboard image or text",
            [this] { CloseMenu(); } },
          { "Drag to select and copy",
            "Release copies text; right-click opens actions",
            [this] { CloseMenu(); } },
          { "Quit", "Ctrl+Q", [this] { CloseMenu(); } }
        };
        m_Option = 0;
      } },
    { "Recover a failed send",
      "Nothing resends automatically",
      [this]
      {
        CloseMenu();
        auto* c = Current();
        if (!c || c->recovery.empty())
        {
          Notify("No failed sends to recover");
          return;
        }
        auto m = c->recovery.back();
        c->recovery.pop_back();
        if (!m.id.empty() && c->messages.count(m.id))
        {
          BeginEdit(*c, m.id);
          c->draft = W(m.text);
          c->cursor = c->draft.size();
          m_Browse = false;
          Notify("Recovered edit · Enter to retry; Esc to cancel");
          return;
        }
        if (!c->draft.empty() && c->draft != W(m.text))
          c->draft += L"\n";
        if (c->draft != W(m.text))
          c->draft += W(m.text);
        c->cursor = c->draft.size();
        if (!m.fileInfo.empty())
        {
          auto f = ProtocolUtil::FileInfoFromHex(m.fileInfo);
          AttachPreview({ f.filePath });
        }
      } },
    { "Update Harbor", "Restart when an installed update is ready", [this] { UpdatePrompt(); } },
    { "Quit Harbor", "Ctrl+Q", [this] { m_Running = false; } }
  };
  m_Dirty = true;
}
void
Harbor::Actions(bool minimal)
{
  if (m_Motion)
    m_AnimatedUntil = TimeUtil::GetCurrentTimeMSec() + 80;
  auto* c = Current();
  if (!c || c->order.empty())
    return;
  if (c->selected.empty())
    c->selected = c->order.back();
  auto it = c->messages.find(c->selected);
  if (it == c->messages.end())
    return;
  auto message = it->second;
  std::string key = m_Current, id = message.id;
  m_Menu = "actions";
  m_Option = 0;
  m_MenuOffset = 0;
  m_Options.clear();
  m_Options.push_back({ "Reply",
                        Name(c->profile, message.senderId),
                        [this, key, id]
                        {
                          CloseMenu();
                          OpenChat(key);
                          auto* target = Current();
                          if (!target->editing.empty())
                            EndEdit(*target);
                          target->reply = id;
                          m_Browse = false;
                        } });
  if (message.isOutgoing)
    m_Options.push_back({ "Edit message",
                          "",
                          [this, key, id]
                          {
                            CloseMenu();
                            OpenChat(key);
                            auto* target = Current();
                            BeginEdit(*target, id);
                            m_Browse = false;
                          } });
  m_Options.push_back({ "Forward",
                        "Choose a destination",
                        [this, message, key]
                        {
                          m_ForwardMessage = message;
                          m_ForwardProfile = m_Chats[key].profile;
                          m_ForwardChat = m_Chats[key].id;
                          ChatPicker(true);
                        } });
  m_Options.push_back({ "Copy text",
                        "",
                        [this, message]
                        {
                          m_Media->CopyText(message.text);
                          CloseMenu();
                          Notify("Copied");
                        } });
  if (!message.fileInfo.empty())
    m_Options.push_back({ "Open / play attachment",
                          "Images preview inline",
                          [this, key, id]
                          {
                            CloseMenu();
                            OpenAttachment(m_Chats[key].profile, m_Chats[key].id, id, true);
                          } });
  auto link = std::regex("https?://[^\\s<>]+");
  std::smatch match;
  if (std::regex_search(message.text, match, link))
  {
    std::string url = match.str();
    m_Options.push_back({ "Open link",
                          url,
                          [this, url]
                          {
                            CloseMenu();
                            m_Media->Open(url);
                          } });
  }
  m_Options.push_back({ "React with a heart",
                        "",
                        [this, key, id, message]
                        {
                          auto r = std::make_shared<SendReactionRequest>();
                          auto& target = m_Chats[key];
                          r->chatId = target.id;
                          r->msgId = id;
                          r->senderId = message.senderId;
                          r->emoji = "❤";
                          Request(target.profile, r);
                          CloseMenu();
                        } });
  if (message.isOutgoing)
    m_Options.push_back({ "Delete message",
                          "Confirmation required",
                          [this, key, id, message]
                          {
                            m_Menu = "delete";
                            m_Option = 0;
                            m_Options = { { "Cancel", "", [this] { CloseMenu(); } },
                                          { "Delete this message",
                                            "This also deletes it on Telegram",
                                            [this, key, id, message]
                                            {
                                              auto r = std::make_shared<DeleteMessageRequest>();
                                              auto& target = m_Chats[key];
                                              r->chatId = target.id;
                                              r->msgId = id;
                                              r->senderId = message.senderId;
                                              Request(target.profile, r);
                                              CloseMenu();
                                            } } };
                          } });
  m_Options.erase(std::remove_if(m_Options.begin(),
                                 m_Options.end(),
                                 [minimal](const Option& option)
                                 {
                                   const bool primary = option.label == "Reply" ||
                                                        option.label == "Forward" ||
                                                        option.label == "Copy text";
                                   return minimal ? !primary : primary;
                                 }),
                  m_Options.end());
  if (!minimal)
    m_Menu = "message-tools";
  m_Dirty = true;
}
void
Harbor::FilePicker(const std::string& directory)
{
  m_Directory = directory;
  m_Menu = "files";
  m_Filter.clear();
  m_Option = 0;
  m_MenuOffset = 0;
  RebuildFiles();
  m_Dirty = true;
}
void
Harbor::RebuildFiles()
{
  m_Options.clear();
  std::string directory = m_Directory;
  const auto path = Expand(S(m_Filter));
  if (!path.empty() && (path[0] == '/' || S(m_Filter)[0] == '~'))
  {
    if (File(path))
    {
      m_Options.push_back(
        { FileUtil::BaseName(path), Size(path), [this, path] { AttachPreview({ path }); } });
      return;
    }
    if (Directory(path))
      directory = path;
  }
  m_Options.push_back(
    { "../", "Parent folder", [this, directory] { FilePicker(FileUtil::DirName(directory)); } });
  DIR* dir = opendir(directory.c_str());
  if (!dir)
    return;
  std::vector<std::string> names;
  while (auto* ent = readdir(dir))
  {
    std::string name = ent->d_name;
    if (name == "." || name == ".." || name[0] == '.')
      continue;
    names.push_back(name);
  }
  closedir(dir);
  std::sort(names.begin(), names.end());
  for (const auto& name : names)
  {
    if (!m_Filter.empty() && directory == m_Directory &&
        StrUtil::ToFold(name).find(StrUtil::ToFold(S(m_Filter))) == std::string::npos)
      continue;
    std::string full = directory + "/" + name;
    bool isDir = Directory(full);
    m_Options.push_back({ name + (isDir ? "/" : ""),
                          isDir ? "folder" : Size(full),
                          [this, full, isDir]
                          {
                            if (isDir)
                              FilePicker(full);
                            else
                              AttachPreview({ full });
                          } });
  }
  m_Option = std::max(0, std::min(m_Option, int(m_Options.size()) - 1));
}
void
Harbor::AttachPreview(std::vector<std::string> files)
{
  if (auto* c = Current())
    if (!c->editing.empty())
    {
      CloseMenu();
      Notify("Finish or cancel editing before attaching a new file");
      return;
    }
  m_Attachments.clear();
  for (auto& file : files)
    if (File(file))
      m_Attachments.push_back(file);
  if (m_Attachments.empty())
  {
    Notify("No readable attachments found");
    return;
  }
  m_Menu = "attachments";
  m_Option = 0;
  m_MenuOffset = 0;
  m_Options = { { "Send " + std::to_string(m_Attachments.size()) + " attachment(s)",
                  "Enter to confirm",
                  [this] { SendAttachments(); } },
                { "Cancel",
                  "Nothing will be sent",
                  [this]
                  {
                    m_Attachments.clear();
                    CloseMenu();
                  } } };
  m_Filter.clear();
  m_Dirty = true;
}
void
Harbor::SendAttachments()
{
  auto* c = Current();
  if (!c)
    return;
  bool first = true;
  for (const auto& file : m_Attachments)
  {
    if (!File(file))
    {
      Notify("A file disappeared; canceled");
      return;
    }
    FileInfo f;
    f.filePath = file;
    f.fileType = FileUtil::GetMimeType(file);
    if (f.fileType.empty())
      f.fileType = Type(file);
    f.fileStatus = FileStatusDownloaded;
    auto r = std::make_shared<SendMessageRequest>();
    r->chatId = c->id;
    r->chatMessage.fileInfo = ProtocolUtil::FileInfoToHex(f);
    if (first)
    {
      if (HarborPaths(c->draft) != m_Attachments)
        r->chatMessage.text = S(c->draft);
      r->chatMessage.quotedId = c->reply;
      first = false;
    }
    Request(c->profile, r);
  }
  c->draft.clear();
  c->cursor = 0;
  c->reply.clear();
  c->follow = true;
  m_Attachments.clear();
  CloseMenu();
  Notify("Sending attachments…");
}
void
Harbor::OpenAttachment(const std::string& p, const std::string& chatId, const std::string& id, bool preview)
{
  auto& c = Ensure(p, chatId);
  auto it = c.messages.find(id);
  if (it == c.messages.end())
    return;
  auto f = ProtocolUtil::FileInfoFromHex(it->second.fileInfo);
  if (!File(f.filePath))
  {
    auto r = std::make_shared<DownloadFileRequest>();
    r->chatId = chatId;
    r->msgId = id;
    r->fileId = f.fileId;
    r->downloadFileAction = DownloadFileActionOpen;
    Request(p, r);
    Notify("Downloading attachment…");
    return;
  }
  auto type = f.fileType.empty() ? Type(f.filePath) : f.fileType;
  if (type.find("audio") == 0)
  {
    if (m_Media->PlayingPath() == f.filePath && m_Media->Playing())
      m_Media->TogglePause();
    else
      m_Media->Play(f.filePath);
    Notify("Voice playback · / to pause or stop");
  }
  else if (type.find("image") == 0 && preview)
  {
    m_Attachments = { f.filePath };
    m_Menu = "preview";
    m_Option = 0;
    m_Options = { { "Open full image",
                    "Default viewer",
                    [this, f]
                    {
                      m_Media->Open(f.filePath);
                      CloseMenu();
                    } },
                  { "Back to conversation", "Esc", [this] { CloseMenu(); } } };
  }
  else
    m_Media->Open(f.filePath);
  m_Dirty = true;
}
void
Harbor::ClipboardPaste()
{
  std::string image = m_Media->PasteboardImage();
  if (!image.empty())
    AttachPreview({ image });
  else
    Paste(W(Clipboard::GetText()));
}
void
Harbor::DrawMenu()
{
  bool picker = m_Menu == "chats" || m_Menu == "forward";
  bool compact = m_Menu == "actions" || m_Menu == "message-tools" || picker;
  int width = std::min(m_MainW - 4, picker ? 50 : compact ? 38 : 64);
  int x = m_MainX + (m_MainW - width) / 2;
  int y =
    4 + (m_AnimatedUntil ? std::max(0, int((m_AnimatedUntil - TimeUtil::GetCurrentTimeMSec()) / 40)) : 0);
  int stride = compact ? 1 : 2;
  int maxItems = std::max(1, std::min(picker ? 8 : 1000, (m_H - 10) / stride));
  int extra = (m_Menu == "attachments" || m_Menu == "preview") ? std::min(12, m_H / 3) : 0;
  int captionHeight = m_Menu == "attachments" ? 4 : 0;
  int optionsHeight = std::min(maxItems, int(m_Options.size())) * stride;
  int height = std::min(m_H - 5, (picker ? 8 : 5) + optionsHeight + extra + captionHeight);
  if (m_Option < m_MenuOffset)
    m_MenuOffset = m_Option;
  if (m_Option >=
      m_MenuOffset +
        std::max(1, std::min(maxItems, (height - (picker ? 8 : 5) - extra - captionHeight) / stride)))
    m_MenuOffset =
      m_Option -
      std::max(1, std::min(maxItems, (height - (picker ? 8 : 5) - extra - captionHeight) / stride)) + 1;
  m_DialogX = x;
  m_DialogY = y;
  m_DialogW = width;
  m_DialogH = height;
  for (int row = y; row < y + height; row++)
    Text(row, x, width, L"", Surface);

  std::string title = m_Menu == "chats"           ? "Find a conversation"
                      : m_Menu == "forward"       ? "Forward to…"
                      : m_Menu == "actions"       ? "Message actions"
                      : m_Menu == "message-tools" ? "Message tools"
                      : m_Menu == "update"        ? "Update ready"
                      : m_Menu == "commands"      ? "Commands & shortcuts"
                      : m_Menu == "files"         ? "Attach · " + m_Directory
                      : m_Menu == "delete"        ? "Delete message?"
                      : m_Menu == "help"          ? "Keyboard help"
                      : m_Menu == "preview"       ? "Image preview"
                                                  : "Preview attachments";
  Text(y + 1, x + 2, width - 4, W(title), Surface, true);
  int start = y + (picker ? 5 : 3);
  bool filtering = m_Menu == "chats" || m_Menu == "forward" || m_Menu == "files";
  if (filtering)
    Text(y + (picker ? 3 : 2),
         x + 2,
         width - 4,
         L"› " + m_Filter + (m_Filter.empty() ? L"search…" : L""),
         Selected);
  if (extra && !m_Attachments.empty())
  {
    auto& file = m_Attachments.front();
    Text(start, x + 2, width - 4, W(FileUtil::BaseName(file) + " · " + Size(file)), Surface);
    int rows = std::max(1, extra - 2);
    int cols = width - 4;
    m_Media->RequestImage(file, cols, rows, Type(file).find("video") == 0);
    auto* image = m_Media->Image(file, cols, rows);
    if (image && !image->failed)
      m_Media->DrawImage(*image, start + 1, x + 2, cols, rows);
    else
      Text(start + 2, x + 2, width - 4, W(Type(file)), Surface);
    start += extra;
  }
  int captionY = -1;
  if (captionHeight)
  {
    captionY = start;
    Text(start, x + 2, width - 4, L"Caption · type to add", Surface);
    auto* c = Current();
    auto layout = Layout(c ? c->draft : L"", std::max(1, width - 6));
    auto point = layout.cursors[c ? std::min(c->cursor, c->draft.size()) : 0];
    int first = std::max(0, point.row - 1);
    for (int i = 0; i < 2; ++i)
      Text(start + 1 + i,
           x + 2,
           width - 4,
           L" " + (first + i < int(layout.lines.size()) ? layout.lines[first + i] : L""),
           Selected);
    start += captionHeight;
  }
  m_OptionY = start;
  int row = start;
  for (int i = m_MenuOffset;
       i < int(m_Options.size()) && row < y + height - (picker ? 3 : 2) && i < m_MenuOffset + maxItems;
       i++, row += stride)
  {
    auto label = W(m_Options[i].label);
    if (picker)
      label = (i == m_Option ? L"› " : L"  ") + label;
    Text(row, x + 2, width - 4, label, i == m_Option ? Selected : Surface, i == m_Option);
    if (picker && !m_Options[i].hint.empty())
      Text(row, x + width - 4, 2, L"●", i == m_Option ? Selected : Surface);
    if (!compact)
      Text(row + 1, x + 3, width - 5, W(m_Options[i].hint), Surface);
    m_Hits.push_back({ row, x + 2, width - 4, "option", std::to_string(i) });
  }
  Text(y + height - 1,
       x + 2,
       width - 4,
       m_Options.empty() ? L"No matches · Esc back" : L"↑↓ · Enter select · Esc close",
       Surface);
  if (captionY >= 0)
  {
    auto* c = Current();
    auto layout = Layout(c ? c->draft : L"", std::max(1, width - 6));
    auto point = layout.cursors[c ? std::min(c->cursor, c->draft.size()) : 0];
    curs_set(1);
    move(captionY + 1 + std::min(1, point.row), x + 3 + point.col);
  }
  else if (filtering)
  {
    curs_set(1);
    move(y + (picker ? 3 : 2), std::min(x + width - 3, x + 4 + Width(m_Filter)));
  }
}
void
Harbor::KeyPress(wint_t k)
{
  m_Dirty = true;
  if (k != KEY_MOUSE && (m_HasSelection || m_MouseDown))
  {
    m_MouseDown = m_MouseDragging = m_HasSelection = false;
    if (k == 27)
      return;
  }
  if (!m_KeyIsSpecial && k >= 32 && k != 127)
  {
    if (!m_Menu.empty())
    {
      if (m_Menu == "attachments")
      {
        Insert(std::wstring(1, wchar_t(k)));
        return;
      }
      if (m_Menu == "chats" || m_Menu == "forward" || m_Menu == "files")
      {
        m_Filter += wchar_t(k);
        m_Option = 0;
        m_MenuOffset = 0;
        if (m_Menu == "files")
          RebuildFiles();
        else
          RebuildPicker();
      }
      return;
    }
    auto* c = Current();
    if (!c)
      return;
    if (k == '/' && c->draft.empty())
    {
      Commands();
      return;
    }
    if (m_Browse && k == ' ' && m_Media->Playing())
    {
      m_Media->TogglePause();
      return;
    }
    Insert(std::wstring(1, wchar_t(k)));
    return;
  }
  if (k == KEY_RESIZE)
  {
    clearok(stdscr, TRUE);
    return;
  }
  if (k == PasteStartKey)
  {
    m_Pasting = true;
    m_Paste.clear();
    m_PasteStart = TimeUtil::GetCurrentTimeMSec();
    return;
  }
  if (k == 17)
  {
    m_Running = false;
    return;
  }
  if (k == 7)
  {
    CloseMenu();
    Latest();
    return;
  }
  if (k == 18)
  {
    auto* c = Current();
    if (c)
    {
      c->fetching = false;
      Fetch(*c);
      Notify("Refreshing conversation…");
    }
    return;
  }

  if (k == KEY_MOUSE)
  {
    Mouse();
    return;
  }
  if (k == FindKey || k == 11)
  {
    ChatPicker();
    return;
  }
  if (!m_Menu.empty())
  {
    if (m_Menu == "attachments" &&
        (k == KEY_BACKSPACE || k == 127 || k == 8 || k == KEY_DC || k == KEY_LEFT || k == KEY_RIGHT ||
         k == KEY_HOME || k == KEY_END || k == 1 || k == 5 || k == 21 || k == DeleteWordKey || k == 23 ||
         k == NewlineKey || k == 10))
    {
      EditKey(k);
      return;
    }
    if (k == 27)
    {
      CloseMenu();
      return;
    }
    if (k == KEY_UP)
    {
      m_Option = std::max(0, m_Option - 1);
      return;
    }
    if (k == KEY_DOWN)
    {
      m_Option = std::min(int(m_Options.size()) - 1, m_Option + 1);
      return;
    }
    if (k == 13 || k == KEY_ENTER)
    {
      Choose(m_Option);
      return;
    }
    if (m_Menu == "chats" || m_Menu == "forward" || m_Menu == "files")
    {
      if (k == KEY_BACKSPACE || k == 127 || k == 8)
      {
        if (!m_Filter.empty())
          m_Filter.pop_back();
      }
      else if (k >= 32 && k < KEY_MIN)
        m_Filter += wchar_t(k);
      else if (k > KEY_MAX && k < 0x110000)
        m_Filter += wchar_t(k);
      m_Option = 0;
      m_MenuOffset = 0;
      if (m_Menu == "files")
        RebuildFiles();
      else
        RebuildPicker();
    }
    return;
  }
  auto* c = Current();
  if (!c)
    return;
  if (k == 27)
  {
    if (!c->editing.empty())
    {
      EndEdit(*c);
      Notify("Edit canceled · draft restored");
      return;
    }
    if (!c->reply.empty())
    {
      c->reply.clear();
      Notify("Reply canceled");
      return;
    }
    if (m_Browse)
      m_Browse = false;
    else
      Select(0);
    return;
  }
  if (k == 22)
  {
    ClipboardPaste();
    return;
  }
  if (k == 20)
  {
    FilePicker(m_Directory);
    return;
  }
  if (k == NewlineKey || k == 10 || k == DeleteWordKey || k == 23)
  {
    m_Browse = false;
    EditKey(k);
    return;
  }
  if (k == KEY_PPAGE || k == KEY_NPAGE)
  {
    c->follow = false;
    c->scroll += k == KEY_PPAGE ? -std::max(1, m_Bottom - m_Top - 2) : std::max(1, m_Bottom - m_Top - 2);
    if (c->scroll < 5)
      Fetch(*c, true);
    c->scroll = std::max(0, c->scroll);
    return;
  }
  if (m_Browse)
  {
    if (k == KEY_UP)
    {
      Select(-1);
      if (c->scroll < 5)
        Fetch(*c, true);
      return;
    }
    if (k == KEY_DOWN)
    {
      Select(1);
      return;
    }
    if (k == 13 || k == KEY_ENTER)
    {
      Actions();
      return;
    }
    if (k == ' ' && m_Media->Playing())
    {
      m_Media->TogglePause();
      return;
    }
    if ((k >= 32 && k < KEY_MIN) || (k > KEY_MAX && k < 0x110000))
    {
      m_Browse = false;
      if (k == '/' && c->draft.empty())
      {
        Commands();
        return;
      }
      EditKey(k);
    }
    return;
  }
  if (k == 13 || k == KEY_ENTER)
  {
    Send();
    return;
  }
  if (k == '/' && c->draft.empty())
  {
    Commands();
    return;
  }
  EditKey(k);
}
bool
Harbor::CellSelected(const CopyCell& cell) const
{
  if (!m_HasSelection)
    return false;
  const auto anchor = std::make_pair(m_SelectionY, m_SelectionX);
  const auto end = std::make_pair(m_SelectionEndY, m_SelectionEndX);
  auto lo = std::min(anchor, end), hi = std::max(anchor, end);
  return std::make_pair(cell.y, cell.x + cell.width - 1) >= lo && std::make_pair(cell.y, cell.x) <= hi;
}
void
Harbor::CopySelection()
{
  std::wstring copied;
  std::string id;
  size_t start = 0, end = 0;
  auto append = [&]
  {
    if (id.empty())
      return;
    auto text = m_CopyTexts.find(id);
    if (text == m_CopyTexts.end() || end <= start || end > text->second.size())
      return;
    if (!copied.empty())
      copied += L"\n\n";
    copied += text->second.substr(start, end - start);
  };
  for (const auto& cell : m_CopyCells)
    if (CellSelected(cell))
    {
      if (cell.id != id)
      {
        append();
        id = cell.id;
        start = cell.start;
      }
      end = cell.end;
    }
  append();
  if (!copied.empty())
  {
    if (m_Media->CopyText(S(copied)))
      Notify("Copied selection");
    else
      Notify("Could not copy selection");
  }
}
void
Harbor::Mouse()
{
  MEVENT e;
  if (getmouse(&e) != OK)
    return;

  auto* c = Current();
  if (e.bstate & (BUTTON3_PRESSED | BUTTON3_CLICKED | BUTTON3_DOUBLE_CLICKED))
  {
    m_MouseDown = m_MouseDragging = m_HasSelection = false;
    if (!m_Menu.empty())
    {
      const bool inside =
        e.x >= m_DialogX && e.x < m_DialogX + m_DialogW && e.y >= m_DialogY && e.y < m_DialogY + m_DialogH;
      if (!inside)
        CloseMenu();
      return;
    }
    for (auto it = m_Hits.rbegin(); it != m_Hits.rend(); ++it)
      if (it->type == "message" && e.y == it->y && e.x >= it->x && e.x < it->x + it->w && c)
      {
        c->selected = it->value;
        m_Browse = true;
        Actions();
        return;
      }
    return;
  }
  if (m_Menu.empty())
  {
    if (e.bstate & (BUTTON4_PRESSED
#ifdef BUTTON5_PRESSED
                    | BUTTON5_PRESSED
#endif
                    ))
      m_MouseDown = m_MouseDragging = m_HasSelection = false;
    if (m_MouseDown)
    {
      m_MouseActivityAt = TimeUtil::GetCurrentTimeMSec();
      m_SelectionEndX = e.x;
      m_SelectionEndY = e.y;
      if (e.x != m_SelectionX || e.y != m_SelectionY)
        m_MouseDragging = true;
      if (m_MouseDragging)
      {
        m_HasSelection = true;
        m_Dirty = true;
      }
      if (e.bstate & BUTTON1_RELEASED)
      {
        m_MouseDown = false;
        if (m_MouseDragging)
        {
          CopySelection();
          m_MouseDragging = false;
          return;
        }
        // Resolve a stationary click on release, so a press can become a drag.
        e.bstate = BUTTON1_CLICKED;
      }
      else
        return;
    }
    else if (e.bstate & BUTTON1_PRESSED)
    {
      m_HasSelection = false;
      for (const auto& hit : m_Hits)
        if (hit.type == "message" && e.y == hit.y && e.x >= hit.x && e.x < hit.x + hit.w)
        {
          m_MouseDown = true;
          m_MouseActivityAt = TimeUtil::GetCurrentTimeMSec();
          m_MouseDragging = false;
          m_SelectionX = m_SelectionEndX = e.x;
          m_SelectionY = m_SelectionEndY = e.y;
          return;
        }
    }
  }
  if (!m_Menu.empty())
  {
    const bool inside =
      e.x >= m_DialogX && e.x < m_DialogX + m_DialogW && e.y >= m_DialogY && e.y < m_DialogY + m_DialogH;
    if (!inside && (e.bstate & (BUTTON1_PRESSED | BUTTON1_CLICKED | BUTTON1_DOUBLE_CLICKED)))
    {
      CloseMenu();
      m_Dirty = true;
      return;
    }
    if (e.bstate & BUTTON4_PRESSED)
    {
      m_Option = std::max(0, m_Option - 1);
      return;
    }
#ifdef BUTTON5_PRESSED
    if (e.bstate & BUTTON5_PRESSED)
    {
      m_Option = std::min(int(m_Options.size()) - 1, m_Option + 1);
      return;
    }
#endif
  }
  if (e.bstate & BUTTON4_PRESSED)
  {
    if (c)
    {
      c->follow = false;
      c->scroll = std::max(0, c->scroll - 3);
      if (c->scroll < 5)
        Fetch(*c, true);
    }
    return;
  }
#ifdef BUTTON5_PRESSED
  if (e.bstate & BUTTON5_PRESSED)
  {
    if (c)
    {
      c->follow = false;
      c->scroll += 3;
    }
    return;
  }
#endif
  if (!(e.bstate & (BUTTON1_PRESSED | BUTTON1_CLICKED | BUTTON1_DOUBLE_CLICKED)))
    return;
  for (auto it = m_Hits.rbegin(); it != m_Hits.rend(); it++)
  {
    const auto& hit = *it;
    if (e.y != hit.y || e.x < hit.x || e.x >= hit.x + hit.w)
      continue;
    if (!m_Menu.empty() && hit.type != "option")
      return;
    if (hit.type == "update")
      UpdatePrompt();
    else if (hit.type == "commands")
      Commands();
    else if (hit.type == "latest")
      Latest();
    else if (hit.type == "chats")
      ChatPicker();
    else if (hit.type == "chat")
      OpenChat(hit.value);
    else if (hit.type == "option")
      Choose(std::stoi(hit.value));
    else if (hit.type == "message" && c)
    {
      c->selected = hit.value;
      // Left-click only establishes the text/keyboard anchor. Message actions
      // belong to right-click; dragging still copies when the button is released.
    }
    return;
  }
  if (e.y >= m_ComposerY && m_Menu.empty())
    m_Browse = false;
}
void
Harbor::KeyDump()
{
  setlocale(LC_ALL, "");
  initscr();
  raw();
  noecho();
  keypad(stdscr, TRUE);
  printw("Harbor key inspector · Ctrl+Q exits\n");
  wint_t key;
  while (get_wch(&key) != ERR)
  {
    if (key == 17)
      break;
    printw("code=%u\n", unsigned(key));
    refresh();
  }
  endwin();
}

Ui::Ui()
  : m_Harbor(std::make_shared<Harbor>())
{
}
Ui::~Ui() = default;
void
Ui::Init()
{
  m_Harbor->Init();
}
void
Ui::Cleanup()
{
  m_Harbor->Cleanup();
}
void
Ui::Run()
{
  m_Harbor->Run();
}
void
Ui::AddProtocol(std::shared_ptr<Protocol> p)
{
  m_Harbor->AddProtocol(p);
}
std::unordered_map<std::string, std::shared_ptr<Protocol>>
Ui::GetProtocols()
{
  return m_Harbor->Protocols();
}
void
Ui::MessageHandler(std::shared_ptr<ServiceMessage> e)
{
  m_Harbor->Receive(e);
}
void
Ui::RunKeyDump()
{
  Harbor::KeyDump();
}
