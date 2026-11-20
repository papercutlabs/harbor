#include "traceprotocol.h"
#include "harbor.h"
#include "wire.h"
#include "input.h"
#include <sys/ioctl.h>
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
#include <fcntl.h>
#include <poll.h>
#include <fstream>
#include <iostream>
#include <regex>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <set>
#include <spawn.h>
#include <sstream>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

extern char** environ;

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
  EmphasisSelected,
  Dialog,
  DialogBorder,
  Backdrop,
  DialogMuted,
  SelectedMuted,
  NoticeBar,
  QuoteShade,
  CodePlain,
  LinkPlain
};
enum
{
  FindKey = KEY_MAX + 101,
  NewlineKey,
  DeleteWordKey,
  BackwardWordKey,
  ForwardWordKey
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
  std::vector<size_t> rowStarts, rowEnds;
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
    out.rowStarts.push_back(start);
    out.rowEnds.push_back(end);
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
// A soft wrap has two visual insertion stops at the same source offset.
// Mouse placement can retain the preceding row's end until the next edit.
TextLayout::Point
CursorPoint(const TextLayout& layout, size_t cursor, bool rowEnd)
{
  if (rowEnd)
    for (size_t row = 0; row < layout.rowEnds.size(); ++row)
      if (layout.rowEnds[row] == cursor)
        return { int(row), Width(layout.lines[row]) };
  return layout.cursors[cursor];
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
std::string
PickerTime(int64_t value)
{
  if (value <= 0)
    return "";
  time_t then = value / 1000, now = TimeUtil::GetCurrentTimeMSec() / 1000;
  struct tm date, today;
  localtime_r(&then, &date);
  localtime_r(&now, &today);
  char text[24];
  const char* format = date.tm_year == today.tm_year && date.tm_yday == today.tm_yday
                         ? "%H:%M"
                         : (now - then < 6 * 86400 && now >= then ? "%a" : "%d/%m");
  strftime(text, sizeof(text), format, &date);
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
// Offline diagnostic recorder: HARBOR_TEST_EVENT_LOG names a file that receives one line per traced
// step of an asynchronous reply (receipt on the protocol thread, deferral, application or the reason
// it was ignored). Unset, nothing is written.
void
TestTrace(const std::string& line)
{
  static const char* path = getenv("HARBOR_TEST_EVENT_LOG");
  if (!path)
    return;
  static std::mutex mutex;
  std::lock_guard<std::mutex> lock(mutex);
  std::ofstream(path, std::ios::app) << TimeUtil::GetCurrentTimeMSec() << ' ' << line << "\n";
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

bool
StrikeDim()
{
  static const bool dim = [] {
    const char* choice = getenv("HARBOR_STRIKE");
    return choice && std::string(choice) == "dim";
  }();
  return dim;
}
// The oldest message of the contiguously loaded history, skipping out-of-sequence visits.
std::string
Contiguous(const std::vector<std::string>& order, const std::set<std::string>& detached)
{
  for (const auto& id : order)
    if (!detached.count(id))
      return id;
  return order.empty() ? std::string() : order.front();
}
}

Harbor::Harbor()
  : m_Media(new HarborMedia)
{
  m_Directory = FileUtil::GetCurrentWorkingDir();
  m_Motion = getenv("HARBOR_REDUCED_MOTION") == nullptr;
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
  if (e->GetMessageType() == SearchMessagesNotifyType)
    TestTrace("receive search token=" + std::to_string(std::static_pointer_cast<SearchMessagesNotify>(e)->token));
  std::lock_guard<std::mutex> lock(m_Mutex);
  m_Events.push_back(e);
  if (e->GetMessageType() == SendMessageNotifyType)
  {
    auto n = std::static_pointer_cast<SendMessageNotify>(e);
    TestTrace("receive send request=" + std::to_string(n->requestId) +
              " pending=" + std::to_string(n->pending) + " success=" + std::to_string(n->success));
  }
}
void
Harbor::Request(const std::string& p, std::shared_ptr<RequestMessage> r)
{
  HarborTrace::Request("ui.request", r);
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
  HarborTrace::Event("ui.init.attempt");
  // SetSession selects the view slot before Init; classic Harbor keeps slot 0.
  LoadState();
  LoadAppearance();
  if (!m_InitialChat.empty())
  {
    m_RestoreChat.clear();
    m_InitialRouteUntil = TimeUtil::GetCurrentTimeMSec() + 30000;
  }
  setlocale(LC_ALL, "");
  m_HaveInitialTerminalMode = tcgetattr(STDIN_FILENO, &m_InitialTerminalMode) == 0;
  initscr();
  raw();
  noecho();
  nonl();
  curs_set(1);
  start_color();
  use_default_colors();
  const std::vector<std::tuple<short, int, int, int>> palette = {
    { 16, 20, 27, 37 },    { 17, 220, 231, 243 }, { 18, 161, 178, 201 },
    { 19, 159, 202, 244 }, { 20, 29, 43, 60 },    { 21, 41, 59, 70 },
    { 22, 185, 213, 184 }, { 23, 52, 71, 95 },    { 24, 224, 193, 152 },
    { 25, 34, 49, 66 }, { 26, 87, 111, 139 }
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
  pair(QuoteShade, 18, 23);
  pair(CodePlain, 22, 16);
  pair(LinkPlain, 19, 16);
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
  pair(Dialog, 17, 25);
  pair(DialogBorder, 19, 25);
  pair(Backdrop, 26, 16);
  // Option descriptions use the muted foreground on the dialog and selection surfaces.
  pair(DialogMuted, 18, 25);
  pair(SelectedMuted, 18, 23);
  // The new-message bar is dark bold text on the light accent; eight-colour terminals use black on cyan.
  init_pair(NoticeBar, COLORS >= 256 ? 16 : COLOR_BLACK, COLORS >= 256 ? 19 : COLOR_CYAN);
  if (!HarborInputStart())
  {
    endwin();
    throw std::runtime_error("Could not start terminal input reader");
  }
  mousemask(ALL_MOUSE_EVENTS | REPORT_MOUSE_POSITION, nullptr);
  mouseinterval(0);
  printf("\033[?1003l\033[?1002h\033[?2004h\033[>1u");
  fflush(stdout);
  m_Ready = true;
  RegisterRestart();
  if (const char* surface = getenv("CMUX_SURFACE_ID"))
    if (!m_UiControl.Start(FileUtil::GetApplicationDir(), surface))
      Notify("This window cannot receive addressed Desk commands. Return to Desk for details.");
  if (!m_Remote) // a window's contacts come from the backend after ConnectNotify
    for (auto& p : m_Protocols)
    {
      MessageCache::FetchContacts(p.first);
    }
}
void
Harbor::FinishUiRoute(const std::string& error)
{
  if (m_ControlReply < 0) return;
  HarborUiControl::Reply(m_ControlReply, error);
  m_ControlReply = -1;
  m_ControlPending = false;
  m_ControlKey.clear();
  m_ControlError = error;
  if (!error.empty())
  {
    m_InitialChat.clear(); m_InitialProfile.clear(); m_InitialRouteUntil = 0;
    m_InitialRouteStatus.clear();
  }
  m_Dirty = true;
}
void
Harbor::BeginUiRoute()
{
  if (!m_ControlPending) return;
  m_ControlPending = false;
  CloseMenu();
  SetInitialChat(m_ControlProfile, m_ControlConversation);
  m_InitialRouteUntil = TimeUtil::GetCurrentTimeMSec() + 30000;
  if (m_Chats.count(m_ControlKey))
  {
    const auto key = m_ControlKey;
    OpenChat(key);
  }
  else
  {
    auto request = std::make_shared<GetChatsRequest>(); request->includeForumTopics = true;
    Request(m_ControlProfile, request);
  }
}
void
Harbor::PollUiControl()
{
  m_UiControl.Poll([this](const HarborRoute::Target& target, int reply) {
    if (m_ControlReply >= 0)
    { HarborUiControl::Reply(reply, "Harbor is still opening an earlier requested conversation. Inspect that window before retrying."); return; }
    if (m_InColdDeskView)
    { HarborUiControl::Reply(reply, "Return from the active legacy Desk view first. No conversation changed."); return; }
    if (m_Menu == "attachments")
    { HarborUiControl::Reply(reply, "Finish or cancel the attachment confirmation in the selected Harbor window first. No conversation changed."); return; }
    std::vector<std::string> profiles;
    bool demo = false;
    for (const auto& profile : m_Protocols)
    { profiles.push_back(profile.first); demo = demo || profile.first.rfind("HarborDemo_", 0) == 0; }
    std::string selected;
    try { selected = HarborRoute::SelectProfile(target, profiles, demo); }
    catch (const std::exception& error) { HarborUiControl::Reply(reply, error.what()); return; }
    m_ControlProfile = selected; m_ControlConversation = target.conversation;
    m_ControlKey = Key(selected, target.conversation); m_ControlReply = reply;
    m_ControlPending = true; m_ControlError.clear();
  });
  if (!m_InDeskView) BeginUiRoute();
}
void
Harbor::SetInitialChat(const std::string& profile, const std::string& conversation)
{
  m_InitialChat = Key(profile, conversation);
  m_InitialProfile = profile;
  m_InitialRouteStatus = "Opening requested conversation…";
}
void
Harbor::SetSession(int p_Slot, std::function<void()> p_OnAuthSuspended)
{
  m_Remote = true;
  m_Slot = p_Slot;
  m_OnAuthSuspended = p_OnAuthSuspended;
}
std::string
Harbor::StatePath() const
{
  if (!m_RestartStateId.empty())
    return FileUtil::GetApplicationDir() + "/harbor-state.window-" + m_RestartStateId;
  const auto stable = StableStatePath();
  return stable.empty() ? LegacyStatePath() : stable;
}
std::string
Harbor::StableStatePath() const
{
  if (HarborDeskUpdate::Path().empty()) return {};
  const char* surface = getenv("CMUX_SURFACE_ID");
  if (!surface || !*surface || strlen(surface) > 96) return {};
  for (const char* ch = surface; *ch; ++ch)
    if (static_cast<unsigned char>(*ch) < 32 || static_cast<unsigned char>(*ch) == 127) return {};
  return FileUtil::GetApplicationDir() + "/harbor-state.surface-" + StrUtil::StrToHex(surface);
}
std::string
Harbor::LegacyStatePath() const
{
  const auto path = FileUtil::GetApplicationDir() + "/harbor-state";
  return m_Slot == 0 ? path : path + "." + std::to_string(m_Slot);
}
void
Harbor::Cleanup()
{
  HarborTrace::Event("ui.cleanup");
  UnregisterRestart();
  if (!m_Ready)
    return;
  FinishUiRoute("The selected Harbor UI ended before opening the requested conversation.");
  m_UiControl.Stop();
  m_DeskHost.Stop();
  if (m_Remote)
    ProcessEvents(); // the connection is frozen; consume final outcomes/recoveries before saving
  SaveState();
  HarborInputStop();
  m_Media->Stop();
  m_Media->ClearGraphics();
  printf("\033[?1002l\033[<u\033[?2004l");
  mousemask(0, nullptr);
  curs_set(1);
  endwin();
  if (m_HaveInitialTerminalMode) tcsetattr(STDIN_FILENO, TCSANOW, &m_InitialTerminalMode);
  fflush(stdout);
  m_Ready = false;
}
void
Harbor::Run()
{
  HarborTrace::Event("ui.run");
  int64_t lastSave = 0;
  bool deferring = false;
  while (m_Running && !AppUtil::IsTerminateRequested())
  {
    TryRestartAll();
    if (!m_Running) break;
    PollUiControl();
    if (!m_MouseDown)
      ProcessEvents();
    if (m_MouseDown != deferring)
    {
      deferring = m_MouseDown;
      TestTrace(deferring ? "defer events: mouse down" : "resume events");
    }
    if (m_AuthSuspended)
    {
      TimeUtil::Sleep(.02);
      continue;
    }
    const auto now = TimeUtil::GetCurrentTimeMSec();
    if (m_DeskStrip.Poll(now)) m_Dirty = true;
    if (m_InitialRouteUntil && now >= m_InitialRouteUntil)
    {
      m_InitialRouteUntil = 0;
      m_InitialRouteStatus = "Requested conversation unavailable. No other chat was opened. Ctrl+R retries; Ctrl+K chooses another.";
      if (m_ControlReply >= 0)
        FinishUiRoute("Conversation unavailable; previous chat and drafts kept");
      m_Dirty = true;
    }
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
    if (auto* c = Current())
    {
      const int phase = c->follow && c->typing.Active() ? (m_Motion ? HarborTyping::Phase(now) : 0) : -1;
      if (phase != m_TypingPhase) { m_TypingPhase = phase; m_Dirty = true; }
    }
    Timeouts(now);
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
    ReadInput();
    if (m_Running && !AppUtil::IsTerminateRequested()) TryDeskUpdate();
  }
}
void
Harbor::TryDeskUpdate()
{
  if (HarborDeskUpdate::Path().empty()) return;
  HarborDeskUpdate::State state;
  state.menu = !m_Menu.empty();
  state.child = m_InDeskView || m_InColdDeskView;
  state.routing = m_ControlPending || m_ControlReply >= 0 || m_InitialRouteUntil != 0 || !m_RestoreChat.empty();
  state.attachments = !m_Attachments.empty() && (m_Menu == "attachments" || m_Menu == "preview");
  state.mouse = m_MouseDown || m_MouseDragging;
  state.browse = m_Browse;
  state.selection = m_HasSelection;
  state.auth = m_AuthSuspended || !m_AuthOwner.empty();
  const auto* current = Current();
  state.latest = current && current->loaded && current->follow && !current->back.set && current->restoreAnchor.empty();
  state.stable = !StableStatePath().empty() && m_StableStateUsable;
  state.draftsEmpty = true;
  const auto empty = [](const Conversation& chat) {
    return chat.draft.empty() && chat.reply.empty() && chat.editing.empty() && chat.draftBeforeEdit.empty() &&
      chat.replyBeforeEdit.empty() && chat.recovery.empty();
  };
  for (const auto& chat : m_Chats) state.draftsEmpty &= empty(chat.second);
  for (const auto& chat : m_SavedStates)
    if (!m_Chats.count(chat.first)) state.draftsEmpty &= empty(chat.second);
  if (!HarborDeskUpdate::Ready(HarborDeskUpdate::Now(), m_LastInput, state)) return;
  const auto path = HarborDeskUpdate::Path();
  const pid_t owner = getppid();
  const auto revision = HarborDeskUpdate::Request(path, owner, TimeUtil::GetCurrentTimeMSec());
  if (revision.empty() || !SaveState(true)) return;
  if (HarborDeskUpdate::Acknowledge(path, owner, revision, TimeUtil::GetCurrentTimeMSec()))
  {
    HarborTrace::Event("desk.update.ready", {{"revision", revision}});
    m_Running = false;
  }
}
void
Harbor::LoadState()
{
  const char* resume = getenv("HARBOR_RESTART_STATE_ID");
  if (resume && strlen(resume) == 32 && std::all_of(resume, resume + 32, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }))
    m_RestartStateId = resume;
  std::ifstream input(StatePath());
  // AcquireSlot holds this UI's numeric slot lock until exit. Never inspect another slot.
  // Read its previous state once; all writes for an update-opted surface use the stable path.
  if (!StableStatePath().empty())
  {
    struct stat info{};
    if (lstat(StatePath().c_str(), &info) == 0)
      m_StableStateUsable = S_ISREG(info.st_mode) && bool(input);
    else if (errno != ENOENT)
      m_StableStateUsable = false;
    else if (m_Remote)
    {
      if (lstat(LegacyStatePath().c_str(), &info) == 0)
      {
        if (S_ISREG(info.st_mode)) input.open(LegacyStatePath());
        m_StableStateUsable = S_ISREG(info.st_mode) && bool(input);
      }
      else if (errno != ENOENT) m_StableStateUsable = false;
    }
  }
  std::string line;
  size_t total = 0;
  while (std::getline(input, line) && total < 8 * 1024 * 1024)
  {
    total += line.size();
    if (line.rfind("recover ", 0) == 0)
    {
      std::istringstream row(line.substr(8));
      std::string key, hex;
      if (row >> key >> hex)
      {
        try
        {
          const auto bytes = StrUtil::StrFromHex(hex);
          HarborWire::Reader reader(bytes);
          ChatMessage message;
          HarborWire::Get(reader, message);
          if (reader.Done()) m_SavedStates[StrUtil::StrFromHex(key)].recovery.push_back(message);
        }
        catch (...) { }
      }
      continue;
    }
    if (line.rfind("browse ", 0) == 0)
    {
      std::istringstream row(line.substr(7));
      row >> m_RestoreBrowse;
      continue;
    }
    if (line.rfind("active ", 0) == 0)
    {
      try
      {
        m_RestoreChat = StrUtil::StrFromHex(line.substr(7));
      }
      catch (...)
      {
      }
      continue;
    }
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
      std::string anchor;
      if (row >> anchor >> c.restoreOffset)
        c.restoreAnchor = anchor == "-" ? "" : StrUtil::StrFromHex(anchor);
      int hasBack = 0;
      if (row >> hasBack && hasBack)
      {
        std::string selectedBack, anchorBack, returnKey;
        auto& mark = c.back;
        if (row >> mark.follow >> mark.browse >> selectedBack >> anchorBack >> returnKey >> mark.scroll >> mark.offset >> mark.landed)
        {
          mark.set = true;
          mark.selected = selectedBack == "-" ? "" : StrUtil::StrFromHex(selectedBack);
          mark.anchor = anchorBack == "-" ? "" : StrUtil::StrFromHex(anchorBack);
          mark.returnKey = returnKey == "-" ? "" : StrUtil::StrFromHex(returnKey);
        }
      }
      int unseen = 0;
      if (row >> c.cursorAtRowEnd >> unseen)
      {
        c.unseen = std::max(0, unseen);
        size_t count = 0;
        std::string id;
        if (row >> count)
          for (size_t i = 0; i < count && row >> id; ++i) c.revealed.insert(StrUtil::StrFromHex(id));
        if (row >> count)
          for (size_t i = 0; i < count && row >> id; ++i) c.expanded.insert(StrUtil::StrFromHex(id));
      }
      m_SavedStates[StrUtil::StrFromHex(key)] = c;
    }
    catch (...)
    {
    }
  }
}
bool
Harbor::SaveState(bool force)
{
  std::map<std::string, Conversation> states = m_SavedStates;
  for (const auto& c : m_Chats)
  {
    auto& saved = states[c.first];
    saved.recovery = c.second.recovery;
    saved.draft = c.second.draft;
    saved.cursor = c.second.cursor;
    saved.cursorAtRowEnd = c.second.cursorAtRowEnd;
    saved.unseen = c.second.unseen;
    saved.revealed = c.second.revealed;
    saved.expanded = c.second.expanded;
    saved.scroll = c.second.scroll;
    saved.follow = c.second.follow;
    saved.selected = c.second.selected;
    saved.reply = c.second.reply;
    saved.editing = c.second.editing;
    saved.draftBeforeEdit = c.second.draftBeforeEdit;
    saved.cursorBeforeEdit = c.second.cursorBeforeEdit;
    saved.replyBeforeEdit = c.second.replyBeforeEdit;
    saved.restoreAnchor = c.second.restoreAnchor;
    saved.restoreOffset = c.second.restoreOffset;
    saved.back = m_ManualRestart ? c.second.back : Conversation::Mark();
    if (c.first == m_Current && !c.second.follow && c.second.restoreAnchor.empty() &&
        c.second.scroll < int(m_Rows.size()))
    {
      int row = c.second.scroll;
      while (row < int(m_Rows.size()) && m_Rows[row].id.empty())
        ++row;
      if (row < int(m_Rows.size()))
      {
        saved.restoreAnchor = m_Rows[row].id;
        int first = row;
        while (first > 0 && m_Rows[first - 1].id == saved.restoreAnchor)
          --first;
        saved.restoreOffset = c.second.scroll - first;
      }
    }
    if (c.second.back.set && !m_ManualRestart)
    {
      // a visit is transient: a restart resumes the reading position the user left for it
      const auto& mark = c.second.back;
      saved.scroll = mark.scroll;
      saved.follow = mark.follow;
      saved.selected = mark.selected;
      saved.restoreAnchor = mark.follow ? std::string() : mark.anchor;
      saved.restoreOffset = mark.offset;
    }
  }
  std::ostringstream out;
  if (m_ManualRestart) out << "browse " << m_Browse << '\n';
  out << "active " << StrUtil::StrToHex(m_RestoreChat.empty() ? m_Current : m_RestoreChat) << '\n';
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
        << (c.replyBeforeEdit.empty() ? "-" : StrUtil::StrToHex(c.replyBeforeEdit)) << ' '
        << (c.restoreAnchor.empty() ? "-" : StrUtil::StrToHex(c.restoreAnchor)) << ' '
        << c.restoreOffset << ' ' << (m_ManualRestart && c.back.set);
    if (m_ManualRestart && c.back.set)
    {
      const auto& mark = c.back;
      out << ' ' << mark.follow << ' ' << mark.browse << ' '
          << (mark.selected.empty() ? "-" : StrUtil::StrToHex(mark.selected)) << ' '
          << (mark.anchor.empty() ? "-" : StrUtil::StrToHex(mark.anchor)) << ' '
          << (mark.returnKey.empty() ? "-" : StrUtil::StrToHex(mark.returnKey)) << ' '
          << mark.scroll << ' ' << mark.offset << ' ' << mark.landed;
    }
    if (m_ManualRestart)
    {
      out << ' ' << c.cursorAtRowEnd << ' ' << c.unseen << ' ' << c.revealed.size();
      for (const auto& id : c.revealed) out << ' ' << StrUtil::StrToHex(id);
      out << ' ' << c.expanded.size();
      for (const auto& id : c.expanded) out << ' ' << StrUtil::StrToHex(id);
    }
    out << '\n';
    for (const auto& message : c.recovery)
    {
      HarborWire::Writer writer;
      HarborWire::Put(writer, message);
      out << "recover " << StrUtil::StrToHex(item.first) << ' ' << StrUtil::StrToHex(writer.buf) << '\n';
    }
  }
  if (!force && out.str() == m_LastState)
    return true;
  const auto path = StatePath();
  std::ofstream file(path + ".tmp", std::ios::trunc);
  if (!file || chmod((path + ".tmp").c_str(), 0600) != 0) return false;
  file << out.str();
  file.close();
  if (file && rename((path + ".tmp").c_str(), path.c_str()) == 0)
  {
    chmod(path.c_str(), 0600);
    if (!m_RestartStateId.empty())
    {
      // Also retain the ordinary slot/surface state for a later fresh launch from the shell.
      const auto stable = StableStatePath();
      const auto ordinary = stable.empty() ? LegacyStatePath() : stable;
      std::ofstream copy(ordinary + ".tmp", std::ios::trunc);
      if (!copy || chmod((ordinary + ".tmp").c_str(), 0600) != 0) return false;
      copy << out.str(); copy.close();
      if (!copy || rename((ordinary + ".tmp").c_str(), ordinary.c_str()) != 0) return false;
    }
    m_LastState = out.str();
    return true;
  }
  return false;
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
Harbor::Merge(Conversation& c, const std::vector<ChatMessage>& messages, bool outOfSequence)
{
  const int64_t previousLatest = c.order.empty() ? -1 : c.messages[c.order.back()].timeSent;
  const auto oldest = Contiguous(c.order, c.detached);
  const int64_t previousOldest = oldest.empty() ? -1 : c.messages[oldest].timeSent;
  for (const auto& m : messages)
  {
    // Count unknown incoming IDs in the current or a later provider second. TDLib adds an unordered
    // message-id tie breaker inside that second; comparing it would miss later live notifications.
    // Initial history, older seconds, existing IDs, own sends and deleted entries do not add to the count.
    if (!c.follow && previousLatest >= 0 && !m.isOutgoing && !m.isDeleted &&
        m.timeSent / 1000 >= previousLatest / 1000 && !c.messages.count(m.id))
      c.unseen++;
    if (!outOfSequence)
      c.detached.erase(m.id);
    else if (!c.messages.count(m.id) && previousOldest >= 0 && m.timeSent < previousOldest)
      c.detached.insert(m.id);
    c.messages[m.id] = m;
    c.last = std::max(c.last, m.timeSent);
    if (m.timeSent >= c.actualLast)
    {
      c.actualLast = m.timeSent;
      const bool attachment = m.text.empty() && !m.fileInfo.empty();
      RichText::Message decoded;
      c.previewNative = !attachment && RichText::Decode(m.richText, decoded);
      c.preview = (m.isOutgoing ? "You: " : "") + (attachment ? "Attachment" : RichText::Preview(m.richText, m.text));
    }
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
        auto chats = std::make_shared<GetChatsRequest>();
        chats->includeForumTopics = true;
        Request(p, chats);
        auto r = std::make_shared<SetStatusRequest>();
        r->isOnline = true;
        Request(p, r);
      }
      else
      {
        m_Connected.erase(p);
        for (auto& entry : m_Chats)
          if (entry.second.profile == p) entry.second.typing.Clear();
        Notify("Connection failed; retry with harbor -s");
      }
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
        int64_t actual =
          info.actualLastMessageTime >= 0
            ? info.actualLastMessageTime
            : (info.lastMessageTime < TimeUtil::GetCurrentTimeMSec() + 86400000 ? info.lastMessageTime : 0);
        if (actual >= c.actualLast)
        {
          c.actualLast = actual;
          if (!info.lastMessageText.empty())
          {
            c.preview = info.lastMessageText;
            c.previewNative = info.lastMessageNative;
          }
        }
        c.unread = info.isUnread;
        if (c.loaded && m_Current == Key(p, info.id) && !c.order.empty() &&
            info.lastMessageTime > c.messages[c.order.back()].timeSent)
          Fetch(c);
      }
      SortChats();
      if (!m_InitialChat.empty())
      {
        if (m_InitialRouteUntil && m_Chats.count(m_InitialChat))
          OpenChat(m_InitialChat);
      }
      else if (!m_RestoreChat.empty() && m_Chats.count(m_RestoreChat))
        OpenChat(m_RestoreChat);
      else if (m_Current.empty() && !m_ChatOrder.empty())
      {
        auto pending = m_RestoreChat;
        OpenChat(m_ChatOrder.front());
        m_RestoreChat = pending;
      }
      break;
    }
    case NewMessagesNotifyType:
    {
      auto n = std::static_pointer_cast<NewMessagesNotify>(e);
      if (!n->previewMsgId.empty())
      {
        Conversation target;
        target.profile = p;
        target.id = n->chatId;
        ChatMessage reference;
        reference.quotedChatId = n->chatId;
        reference.quotedId = n->previewMsgId;
        auto& original = m_ReplyOriginals[ReplyKey(target, reference)];
        original.pending = false;
        original.chat = n->chatId;
        original.found = n->success && !n->chatMessages.empty() &&
                         n->chatMessages.front().id == n->previewMsgId &&
                         !n->chatMessages.front().isDeleted;
        if (original.found)
          original.message = n->chatMessages.front();
        auto visitFrom = original.visitFrom;
        original.visitFrom.clear();
        if (!visitFrom.empty() && m_Current == visitFrom)
        {
          if (original.found)
            VisitReply(visitFrom, reference);
          else
            Notify("The original message is unavailable");
        }
        break;
      }
      auto& c = Ensure(p, n->chatId);
      const auto oldest = Contiguous(c.order, c.detached);
      int previousRows = 0;
      const bool older = n->sequence && !n->fromMsgId.empty();
      if (older && !c.follow && m_Current == Key(p, c.id))
      {
        BuildRows();
        previousRows = int(m_Rows.size());
      }
      // Only a newly received incoming message ends that sender's activity. Cache/history replies
      // and duplicate updates must not clear a newer typing pulse.
      if (n->success && !n->cached && n->fromMsgId.empty())
        for (const auto& message : n->chatMessages)
          if (!message.isOutgoing && !message.isDeleted && !c.messages.count(message.id))
            c.typing.Complete(message.senderId, message.timeSent);
      Merge(c, n->chatMessages, !n->sequence);
      if (n->success)
        c.loaded = true;
      // an original arriving after the user left its conversation is not kept above loaded history
      if (!n->sequence && m_Current != Key(p, c.id) && !c.back.set)
        DropDetached(c);
      // an explicit failure of the single-message fetch answers a pending visit at once
      if (!n->sequence && !n->success && n->chatMessages.empty() && !c.visit.empty() && c.visitUntil &&
          m_Current == Key(p, c.id))
        VisitFailed(c);
      if (n->sequence)
      {
        c.fetching = false;
        if (!n->success)
        {
          c.historyStalls = 3;
          if (m_Current == Key(p, c.id))
            Notify("Could not load this conversation · Ctrl+R to retry");
          break;
        }
        if (n->success && older && n->chatMessages.empty())
          c.more = false;
        if (older && !n->chatMessages.empty() && !c.order.empty() && Contiguous(c.order, c.detached) == oldest)
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
      // after the history scroll adjustment, so a page landing with the original does not move it
      if (!c.visit.empty() && c.messages.count(c.visit) && m_Current == Key(p, c.id))
        Visit(m_Current, c.visit);
      break;
    }
    case SendMessageNotifyType:
    {
      auto n = std::static_pointer_cast<SendMessageNotify>(e);
      if (n->pending)
        break; // local TDLib acceptance is not a delivery outcome
      if (n->success)
      {
        if (!n->chatMessage.id.empty())
          Merge(Ensure(p, n->chatId), { n->chatMessage });
        m_Status.clear();
        m_StatusUntil = 0;
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
        Notify("Send failed · draft restored; Ctrl+/ → Recover");
      }
      break;
    }
    case DeleteMessageNotifyType:
    {
      auto n = std::static_pointer_cast<DeleteMessageNotify>(e);
      if (n->success)
      {
        auto& c = Ensure(p, n->chatId);
        auto& deleted = c.messages[n->msgId];
        deleted.id = n->msgId;
        deleted.isDeleted = true;
        ChatMessage reference;
        reference.quotedChatId = c.id;
        reference.quotedId = n->msgId;
        auto& original = m_ReplyOriginals[ReplyKey(c, reference)];
        original.pending = original.found = false;
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
        else if (n->downloadFileAction == DownloadFileActionSave)
        {
          if (File(ProtocolUtil::FileInfoFromHex(n->fileInfo).filePath))
            SaveAttachment(p, n->chatId, n->msgId);
          else
            Notify("Could not download the attachment");
        }
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
      Ensure(p, n->chatId).typing.Update(n->userId, n->isTyping, TimeUtil::GetCurrentTimeMSec());
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
        HarborInputStop();
        endwin();
        if (m_HaveInitialTerminalMode) tcsetattr(STDIN_FILENO, TCSANOW, &m_InitialTerminalMode);
        m_AuthSuspended = true;
        if (m_OnAuthSuspended)
          m_OnAuthSuspended(); // lend this terminal to the session backend's native prompt
      }
      else if (m_AuthOwner == p)
      {
        m_AuthOwner.clear();
        m_AuthSuspended = false;
        if (!HarborInputStart())
          throw std::runtime_error("Could not resume terminal input reader");
        m_LastInput = HarborDeskUpdate::Now();
        refresh();
      }
      break;
    }
    case SearchMessagesNotifyType:
      SearchResults(std::static_pointer_cast<SearchMessagesNotify>(e));
      break;
    case AvailableReactionsNotifyType:
      ReactionResults(std::static_pointer_cast<AvailableReactionsNotify>(e));
      break;
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
    r->fromMsgId = Contiguous(c.order, c.detached);
  c.fetching = true;
  Request(c.profile, r);
}
void
Harbor::OpenChat(const std::string& key)
{
  HarborTrace::Event("ui.chat.open");
  auto it = m_Chats.find(key);
  if (it == m_Chats.end())
    return;
  if (auto* previous = Current(); previous && !previous->follow &&
      previous->restoreAnchor.empty() && previous->scroll < int(m_Rows.size()))
  {
    int row = previous->scroll;
    while (row < int(m_Rows.size()) && m_Rows[row].id.empty())
        ++row;
    if (row < int(m_Rows.size()))
    {
      previous->restoreAnchor = m_Rows[row].id;
      int first = row;
      while (first > 0 && m_Rows[first - 1].id == previous->restoreAnchor)
        --first;
      previous->restoreOffset = previous->scroll - first;
    }
  }
  m_MouseDown = m_MouseDragging = m_HasSelection = false;
  if (auto* previous = Current(); previous && m_Current != key)
  {
    // a pending visit belongs to the conversation being left: its original may still arrive and
    // stay loaded, but it is neither shown later nor reported unavailable there
    previous->visit.clear();
    previous->visitUntil = 0;
    for (auto& original : m_ReplyOriginals)
      if (original.second.visitFrom == m_Current)
        original.second.visitFrom.clear();
    if (previous->back.set && !previous->back.landed)
      previous->back = Conversation::Mark();
    if (!previous->back.set)
      DropDetached(*previous);
  }
  if (m_Current != key)
    m_Search = m_Reactions = Pending();
  const bool restoreBrowse = !m_RestoreChat.empty() && m_RestoreChat == key && m_RestoreBrowse;
  if (m_RestoreChat == key) m_RestoreBrowse = false;
  m_Current = key;
  m_RestoreChat.clear();
  m_InitialChat.clear();
  m_InitialProfile.clear();
  m_InitialRouteStatus.clear();
  m_InitialRouteUntil = 0;
  if (m_ControlReply >= 0)
    FinishUiRoute(m_Current == m_ControlKey ? std::string() : "Requested route was cancelled by another chat selection. Drafts were kept.");
  else m_ControlError.clear();
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
  m_Browse = restoreBrowse;
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
  m_CopyDecor.clear();
  auto* c = Current();
  if (!c)
    return;
  int width = std::max(8, m_MainW - 10);
  std::string previousDay;
  for (const auto& id : c->order)
  {
    // Keep fetched originals for reply excerpts, but only draw detached history during a visit.
    if (c->detached.count(id) && !c->back.set)
      continue;
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
    {
      std::wstring excerpt = ReplyExcerpt(*c, m);
      int room = std::max(4, width - 2);
      if (Width(excerpt) > room)
        excerpt = Pad(excerpt, room - 1) + L"…";
      Row quote{ (m_QuoteAppearance == 1 ? L"│ " : L"") + excerpt, id, m.isOutgoing, false };
      quote.quote = true;
      m_Rows.push_back(std::move(quote));
    }
    if (!m.text.empty() || !m.richText.empty())
    {
      auto formatted = Format(*c, m);
      m_CopyTexts[id] = formatted.text;
      m_CopyDecor[id] = formatted.decor;
      auto layout = Layout(formatted.text, width);
      std::map<std::pair<int, int>, unsigned> styles;
      for (size_t i = 0; i < formatted.text.size(); ++i)
        if (!iswspace(formatted.text[i]))
          styles[{ layout.cursors[i].row, layout.cursors[i].col }] |= formatted.styles[i];
      // display-only gaps after list items and headings: same message, no source characters
      std::set<int> spaced;
      for (auto at : HarborFormatting::Spacers(formatted))
        spaced.insert(layout.cursors[at].row);
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
        if (spaced.count(int(r)))
          m_Rows.push_back({ L"", id, m.isOutgoing, false });
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
        label += " · click to download";
      if (f.fileStatus == FileStatusDownloaded)
        label += " · click to save";
      Row attachment{ W(label), id, m.isOutgoing, false };
      attachment.attachment = true;
      m_Rows.push_back(std::move(attachment));
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
    if (m_TimestampPosition == 1)
    {
      Row stamp{ W(time), id, m.isOutgoing, false };
      stamp.timestamp = true;
      m_Rows.push_back(std::move(stamp));
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
  const int stripHeight = m_DeskStrip.Enabled() ? 4 : 0;
  if (m_W < 24 || m_H < 12 + stripHeight)
  {
    erase();
    m_Hits.clear();
    m_CopyCells.clear();
    for (int row = 0; row < stripHeight; ++row)
      Text(row, 0, m_W, m_DeskStrip.Rows()[row], Muted);
    Text(stripHeight, 0, m_W, L"Harbor · enlarge terminal", Muted);
    curs_set(0);
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
  // Approved indicator A: one count-only bar in the spacer row directly above the composer (and any
  // reply context), so neither the draft nor the reading position moves when it appears.
  bool newBar = c && !c->follow && c->unseen > 0;
  const int newBarY = m_Bottom;
  // Keep this row reserved even while idle. The existing final history margin now separates
  // the latest message from the bubble, without moving messages when activity starts or stops.
  const int typingY = m_Bottom - 1;
  --m_Bottom;
  const int chatY = stripHeight + ((stripHeight || m_CommandHint == 8) ? 1 : 0);
  m_Top = 3 + chatY;
  bkgd(COLOR_PAIR(Base));
  erase();
  m_Hits.clear();
  m_CopyCells.clear();
  // The current chat is the first row; only a not-yet-connected state takes space on the account row.
  const int titleHint = m_CommandHint == 0 ? 17 : 0;
  Text(chatY, m_MainX + 2, m_MainW - 4 - titleHint, c ? W(c->name) : L"Connecting…", Accent, true);
  const std::wstring connection = !m_AuthOwner.empty()  ? L"● authenticating"
                                  : m_Connected.empty() ? L"○ connecting"
                                                        : L"";
  Text(chatY + 1,
       m_MainX + 2,
       m_MainW - 4 - (connection.empty() ? 0 : Width(connection) + 2),
       c ? W(m_Protocols[c->profile]->GetProfileDisplayName()) : L"",
       Muted);
  if (!connection.empty())
    Text(chatY + 1, std::max(m_MainX + 2, m_W - 2 - Width(connection)), Width(connection), connection, Warning);
  if (m_MainX)
  {
    for (int y = stripHeight; y < m_H; y++)
      Text(y, 24, 1, L"│", Muted);
    Text(stripHeight, 2, 20, L"CONVERSATIONS", Muted);
    int y = stripHeight + 2;
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
    if (!c->follow && !c->restoreAnchor.empty() && c->loaded)
    {
      auto anchor = std::find_if(m_Rows.begin(), m_Rows.end(), [&](const Row& row) {
        return row.id == c->restoreAnchor;
      });
      if (anchor != m_Rows.end())
      {
        c->scroll = std::max(0, int(anchor - m_Rows.begin()) + c->restoreOffset);
        c->restoreAnchor.clear();
      }
      else if (c->more && c->historyStalls < 3 && !c->fetching &&
               TimeUtil::GetCurrentTimeMSec() >= c->historyRetryAt)
        Fetch(*c, true, true);
      else if (!c->more)
        c->restoreAnchor.clear();
    }
    if (c->follow)
    {
      c->unseen = 0;
      c->scroll = std::max(0, int(m_Rows.size()) - visible);
    }
    c->scroll = std::max(0, std::min(c->scroll, std::max(0, int(m_Rows.size()) - visible)));
    // Reading down to the newest row sees what was new.
    if (newBar && c->scroll >= int(m_Rows.size()) - visible)
    {
      c->unseen = 0;
      newBar = false;
    }
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
      const int color = selected ? Selected : row.quote && m_QuoteAppearance == 2 ? QuoteShade :
                        m_MessageAppearance == 1 ? Base : row.sent ? Sent : Received;
      if (row.id.empty())
      {
        Text(y, m_MainX + 2, m_MainW - 4, row.text, Muted);
        continue;
      }
      Fill(y, x, w, color);
      Text(y,
           x + 2,
           w - 4,
           (row.header || row.timestamp || (row.text.size() && row.text[0] == 1)) ? L"" : row.text,
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
            pair = selected ? Selected : m_MessageAppearance == 1 ? Muted : row.sent ? MetadataOut : Metadata;
          if (style & Emphasis)
          {
            pair = selected ? EmphasisSelected : m_MessageAppearance == 1 ? LinkPlain : row.sent ? LinkOut : LinkIn;
            attr |= A_BOLD;
          }
          if (style & Code)
            pair = selected ? CodeSelected : m_MessageAppearance == 1 ? CodePlain : row.sent ? CodeOut : CodeIn;
          if (style & Link)
          {
            pair = selected ? LinkSelected : m_MessageAppearance == 1 ? LinkPlain : row.sent ? LinkOut : LinkIn;
            attr |= A_UNDERLINE;
          }
          if (style & Underline)
            attr |= A_UNDERLINE;
          if (style & Spoiler)
            pair = selected ? Selected : m_MessageAppearance == 1 ? Muted : row.sent ? MetadataOut : Metadata;
#ifdef A_ITALIC
          if (style & Italic)
            attr |= A_ITALIC;
#else
          if (style & Italic)
            attr |= A_UNDERLINE;
#endif
          attrset(COLOR_PAIR(pair) | attr);
          if ((style & Strike) && Width(row.text[j]) > 0 && row.text[j] != L' ' && !StrikeDim())
          {
            // ncurses has no strikethrough attribute; a combining long stroke overlay is visible
            // in ordinary terminals. HARBOR_STRIKE=dim selects the dimmed fallback instead.
            wchar_t struck[] = { row.text[j], L'\u0336', 0 };
            cchar_t cell;
            setcchar(&cell, struck, attr, short(pair), nullptr);
            mvadd_wch(y, x + 2 + col, &cell);
          }
          else
          {
            if (style & Strike)
              attrset(COLOR_PAIR(selected ? Selected : row.sent ? MetadataOut : Metadata) | attr | A_DIM);
            mvaddnwstr(y, x + 2 + col, &row.text[j], 1);
          }
          col += Width(row.text[j]);
        }
      }
      if (row.timestamp)
        Text(y, x + w - 2 - std::min(w - 4, Width(row.text)), std::min(w - 4, Width(row.text)),
             row.text, selected ? Selected : m_MessageAppearance == 1 ? Muted : row.sent ? MetadataOut : Metadata);
      if (row.header)
      {
        const auto& message = c->messages[row.id];
        std::wstring name = W(message.isOutgoing ? "You" : Name(c->profile, message.senderId));
        auto time = W(Time(message.timeSent) + (message.isOutgoing ? Receipt(message.isRead) : "") +
                      (message.isEdited ? " · edited" : ""));
        const int tw = m_TimestampPosition == 0 ? std::min((w - 4) / 2, Width(time)) : 0;
        Text(y,
             x + 2,
             std::max(1, w - tw - 5),
             name,
             selected ? Selected : m_MessageAppearance == 1 ? Accent : row.sent ? SenderOut : Sender,
             true);
        if (tw) Text(y, x + w - tw - 2, tw, time, selected ? Selected : m_MessageAppearance == 1 ? Muted : row.sent ? MetadataOut : Metadata);
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
      if (row.attachment)
        m_Hits.push_back({ y, x, w, "attachment", row.id });
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
    // The reserved row and the history spacer above it stay present in idle and active layouts.
    if (c->follow && c->typing.Active())
    {
      const int x = m_MainX + 2, y = typingY;
      Fill(y, x, 11, Received);
      for (int dot = 0; dot < 3; ++dot)
      {
        const bool bright = !m_Motion || dot == HarborTyping::Phase(TimeUtil::GetCurrentTimeMSec());
        attrset(COLOR_PAIR(Received) | (bright ? A_BOLD : A_DIM));
        // Font weight/dim can render identically in a terminal. Different-size glyphs make
        // the traveling pulse visible while keeping all three dots and a fixed cell width.
        mvaddnwstr(y, x + 3 + dot * 2, bright ? L"●" : L"•", 1);
      }
    }
    if (!c->reply.empty() || !c->editing.empty())
      Text(m_ComposerY - 1,
           m_MainX + 2,
           m_MainW - 4,
           W(!c->editing.empty() ? "Editing your message · Esc cancels" : "Replying · " + c->reply),
           Accent);
    for (int row = 0; row < m_ComposerRows + 2; ++row)
      Fill(m_ComposerY + row, m_MainX + 2, m_MainW - 4, Surface);
    auto point = CursorPoint(draftLayout, std::min(c->cursor, c->draft.size()), c->cursorAtRowEnd);
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
  {
    Text(m_Top + 1, m_MainX + 2, m_MainW - 4,
         m_InitialRouteStatus.empty() ? L"Waiting for your conversations…" : W(m_InitialRouteStatus), Muted);
  }
  if (m_Browse)
    Text(m_H - 2, m_MainX + 2, std::max(1, m_MainW - 21), L"MESSAGES · ↑↓ · Enter", Muted);
  {
    const bool typing = c && !c->draft.empty();
    std::wstring hint = L"Ctrl+/ Commands";
    int y = chatY, x = m_W - 17;
    switch (m_CommandHint)
    {
      case 1: y = m_ComposerY; break;
      case 2: y = m_ComposerY; break;
      case 3: y = m_ComposerY; x = m_MainX + 2; break;
      case 4: y = m_ComposerY + 1; x = m_MainX + 6; if (typing) hint.clear(); break;
      case 5: y = m_H - 2; hint = L"[Ctrl+/] Commands"; x = m_W - 19; break;
      case 6: y = m_ComposerY + 1; if (typing) hint = L"Ctrl+/"; x = m_W - Width(hint) - 3; break;
      case 7: y = m_ComposerY + 1; hint = L"Ctrl+/"; x = m_W - 9; break;
      case 8: y = stripHeight; x = m_MainX + 2; break;
    }
    // Prompt-side hints use only empty cells after the current draft, never cover typed text.
    if ((m_CommandHint == 6 || m_CommandHint == 7) && typing &&
        (draftLines.size() > 1 || Width(draftLines[0]) + m_MainX + 8 >= x)) hint.clear();
    if (m_CommandHint == 1)
      Text(m_ComposerY, m_MainX + 2, m_MainW - 4, std::wstring(m_MainW - 4, L'─'), Muted);
    if (!hint.empty())
    {
      x = std::max(m_MainX + 2, x);
      Text(y, x, std::min(Width(hint), m_W - x - 1), hint,
           (m_CommandHint == 4 || m_CommandHint == 6 || m_CommandHint == 7) ? Surface : Muted);
      m_Hits.push_back({ y, x, std::min(Width(hint), m_W - x - 1), "commands", "" });
    }
  }
  Text(m_H - 1, m_MainX + 2, m_MainW - 4, W(m_Status), m_Status.empty() ? Muted : Good);
  if (!m_UpdateTarget.empty())
  {
    Text(m_H - 1, m_MainX + 2, m_MainW - 4, W("↑ Harbor " + m_UpdateVersion + " ready · Ctrl+/ → Update"), Accent);
    m_Hits.push_back({ m_H - 1, m_MainX + 2, m_MainW - 4, "update", "" });
  }
  if (newBar)
  {
    const int x = m_MainX + 2, w = std::max(1, m_MainW - 4);
    const std::wstring count = W("↓ " + std::to_string(c->unseen) + (c->unseen == 1 ? " new message" : " new messages"));
    const std::wstring shortCount = W("↓ " + std::to_string(c->unseen) + " new");
    const std::wstring key = L"Ctrl+G";
    Fill(newBarY, x, w, NoticeBar);
    const std::wstring label = Width(count) + Width(key) + 4 <= w ? count : shortCount;
    Text(newBarY, x + 1, std::max(1, w - 2), label, NoticeBar, true);
    if (Width(label) + Width(key) + 4 <= w)
      Text(newBarY, x + w - 1 - Width(key), Width(key), key, NoticeBar, true);
    m_Hits.push_back({ newBarY, x, w, "latest", "" });
  }
  else if (c && !c->follow && c->scroll + std::max(1, m_Bottom - m_Top) < int(m_Rows.size()) - 2)
  {
    Text(m_H - 2, m_MainX + 2, std::max(1, m_MainW - 21), L"↓ Latest · Ctrl+G", Accent);
    m_Hits.push_back({ m_H - 2, m_MainX + 2, std::max(1, m_MainW - 21), "latest", "" });
  }
  if (!m_ControlError.empty())
    Text(m_H - 1, m_MainX + 2, m_MainW - 4, W(m_ControlError), Warning);
  else if (m_ControlReply >= 0)
    Text(m_H - 1, m_MainX + 2, m_MainW - 4, L"Opening requested conversation…", Accent);
  if (!m_Menu.empty())
    DrawMenu();
  for (int row = 0; row < stripHeight; ++row)
    Text(row, 0, m_W, m_DeskStrip.Rows()[row], Muted);
  if (c && m_Menu.empty() && !m_Browse)
  {
    auto point = CursorPoint(draftLayout, std::min(c->cursor, c->draft.size()), c->cursorAtRowEnd);
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
  SetTitle(c ? c->name : "Harbor");
  m_Media->EndFrame();
}
// The terminal title (OSC 2) names the current chat on this UI's own terminal and is written only when it changes.
std::string
Harbor::TerminalTitle(const std::string& name)
{
  std::string out;
  for (auto ch : W(name))
  {
    if (ch < 0x20 || (ch >= 0x7f && ch <= 0x9f) || ch == 0x2028 || ch == 0x2029)
    {
      if (!out.empty() && out.back() != ' ')
        out += ' ';
      continue;
    }
    out += S(std::wstring(1, ch));
  }
  while (!out.empty() && out.back() == ' ')
    out.pop_back();
  return out.empty() ? "Harbor" : out;
}
void
Harbor::SetTitle(const std::string& name)
{
  const auto title = TerminalTitle(name);
  if (title == m_Title)
    return;
  m_Title = title;
  printf("\033]2;%s\007", title.c_str());
  fflush(stdout);
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
  c->cursorAtRowEnd = false;
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
  const bool cursorAtRowEnd = c->cursorAtRowEnd;
  c->cursorAtRowEnd = false;
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
  else if (k == BackwardWordKey)
  {
    // Match Ctrl+W: whitespace separates words; punctuation stays with its word.
    while (c->cursor && iswspace(c->draft[c->cursor - 1]))
      --c->cursor;
    while (c->cursor && !iswspace(c->draft[c->cursor - 1]))
      --c->cursor;
  }
  else if (k == ForwardWordKey)
  {
    while (c->cursor < c->draft.size() && iswspace(c->draft[c->cursor]))
      ++c->cursor;
    while (c->cursor < c->draft.size() && !iswspace(c->draft[c->cursor]))
      ++c->cursor;
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
    // Use the same layout as the active composer, including caption previews.
    const int width = m_Menu == "attachments" ? m_DialogW - 6 : m_MainW - 11;
    const auto layout = Layout(c->draft, std::max(1, width));
    const auto point = CursorPoint(layout, c->cursor, cursorAtRowEnd);
    const size_t start = layout.rowStarts[point.row];
    c->draft.erase(start, c->cursor - start);
    c->cursor = start;
  }
  else if (k == KEY_UP || k == KEY_DOWN)
  {
    auto layout = Layout(c->draft, std::max(1, m_MainW - 11));
    auto point = CursorPoint(layout, c->cursor, cursorAtRowEnd);
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
  else if (k > KEY_MAX && k <= 0x10ffff && k != FindKey && k != NewlineKey)
  {
    Insert(std::wstring(1, wchar_t(k)));
  }
  m_Dirty = true;
}
void
Harbor::BeginEdit(Conversation& c, const std::string& id)
{
  HarborTrace::Event("ui.message.edit");
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
  HarborTrace::Event("ui.send.attempt");
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
  m.richText.clear();
  m.quotedId = c->reply;
  m.quotedChatId = c->id;
  if (!c->reply.empty())
  {
    auto it = c->messages.find(c->reply);
    if (it != c->messages.end())
    {
      m.quotedText = it->second.text;
      m.quotedSender = it->second.senderId;
      m.quotedRichText = it->second.richText;
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
  HarborTrace::Event("ui.chat_picker.open");
  m_Menu = forward ? "forward" : "chats";
  if (m_Motion)
    m_AnimatedUntil = TimeUtil::GetCurrentTimeMSec() + 80;
  m_Filter.clear();
  m_Option = 0;
  m_MenuOffset = 0;
  RebuildPicker(false);
  m_Dirty = true;
}
void
Harbor::RebuildPicker(bool preserveSelection)
{
  std::string selected;
  if (preserveSelection && m_Option >= 0 && m_Option < int(m_Options.size()))
    selected = m_Options[m_Option].key;
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
                              auto body = m_ForwardText;
                              std::string source = Name(m_ForwardProfile, message.senderId);
                              CloseMenu();
                              OpenChat(key);
                              auto* target = Current();
                              if (target)
                              {
                                std::wstring draft = L"Forwarded from " + W(source) + L"\n" + body;
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
    m_Options.back().key = key;
    m_Options.back().time = PickerTime(c.actualLast);
    // a native preview is already plain, concealed text; only legacy markup is parsed
    m_Options.back().preview =
      S(Safe(c.previewNative ? W(c.preview) : HarborFormatting::Parse(W(c.preview)).text));
  }
  if (!selected.empty())
    for (size_t i = 0; i < m_Options.size(); ++i)
      if (m_Options[i].key == selected)
      {
        m_Option = int(i);
        break;
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
  HarborTrace::Event("ui.update.open");
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
  m_Options = { { "Restart",
                  "",
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
                { "Later", "", [this] { CloseMenu(); } } };
  m_Dirty = true;
}
// Appearance preferences are shared by this configuration's windows, separate from drafts.
void
Harbor::LoadAppearance()
{
  std::ifstream in(FileUtil::GetApplicationDir() + "/harbor-appearance");
  std::string key;
  int value;
  while (in >> key >> value)
  {
    if (key == "quote" && value >= 0 && value < 3) m_QuoteAppearance = value;
    if (key == "messages" && value >= 0 && value < 2) m_MessageAppearance = value;
    if (key == "timestamp" && value >= 0 && value < 2) m_TimestampPosition = value;
    if (key == "commands" && value >= 0 && value < 9) m_CommandHint = value;
  }
}
bool
Harbor::SaveAppearance()
{
  const auto path = FileUtil::GetApplicationDir() + "/harbor-appearance";
  const auto temporary = path + "." + std::to_string(getpid());
  std::ofstream out(temporary, std::ios::trunc);
  out << "quote " << m_QuoteAppearance << "\nmessages " << m_MessageAppearance
      << "\ntimestamp " << m_TimestampPosition << "\ncommands " << m_CommandHint << "\n";
  out.close();
  if (!out || rename(temporary.c_str(), path.c_str()) != 0)
  { unlink(temporary.c_str()); Notify("Could not save appearance settings"); return false; }
  chmod(path.c_str(), 0600);
  return true;
}
void
Harbor::Settings()
{
  LoadAppearance();
  m_Menu = "settings";
  m_Filter.clear(); m_Option = 0; m_MenuOffset = 0;
  const std::vector<std::string> quotes = { "Plain", "Left rule", "Shaded" };
  const std::vector<std::string> hints = { "Chat title", "Composer divider", "Above prompt right", "Above prompt left",
    "Empty prompt", "Footer keycap", "Contracts while typing", "Prompt key only", "Between quota and chat" };
  m_Options = {
    { "Quoted replies", quotes[m_QuoteAppearance], [this] { AppearanceChoices("quote"); } },
    { "Messages", m_MessageAppearance ? "Plain" : "Panels", [this] { AppearanceChoices("messages"); } },
    { "Timestamps", m_TimestampPosition ? "Bottom right" : "Top right", [this] { AppearanceChoices("timestamp"); } },
    { "Commands hint", hints[m_CommandHint], [this] { AppearanceChoices("commands"); } },
    { "Update all windows", "Keep chats and drafts", [this] { UpdateAll(); } }
  };
  m_Dirty = true;
}
void
Harbor::AppearanceChoices(const std::string& field)
{
  m_Menu = "appearance"; m_Options.clear(); m_Option = 0; m_MenuOffset = 0;
  std::vector<std::string> labels;
  int selected = 0;
  if (field == "quote") { labels = { "Plain", "Left rule", "Shaded" }; selected = m_QuoteAppearance; }
  if (field == "messages") { labels = { "Panels", "Plain" }; selected = m_MessageAppearance; }
  if (field == "timestamp") { labels = { "Top right", "Bottom right" }; selected = m_TimestampPosition; }
  if (field == "commands")
  {
    labels = { "Chat title", "Composer divider", "Above prompt right", "Above prompt left", "Empty prompt",
      "Footer keycap", "Contracts while typing", "Prompt key only", "Between quota and chat" };
    selected = m_CommandHint;
  }
  for (size_t i = 0; i < labels.size(); ++i)
    m_Options.push_back({ labels[i], int(i) == selected ? "Selected" : "", [this, field, i] {
      if (field == "quote") m_QuoteAppearance = i;
      if (field == "messages") m_MessageAppearance = i;
      if (field == "timestamp") m_TimestampPosition = i;
      if (field == "commands") m_CommandHint = i;
      SaveAppearance(); CloseMenu();
    } });
  m_Dirty = true;
}
void
Harbor::Commands()
{
  HarborTrace::Event("ui.commands.open");
  m_Menu = "commands";
  m_Filter.clear();
  m_Option = 0;
  m_MenuOffset = 0;
  m_Options = {
    { "More message tools", "For the selected message", [this] { Actions(false); } },
    { "Search or pinned messages", "Searches Telegram, not just loaded messages", [this] { SearchPrompt(); } },
    { "Chats", "Ctrl+K", [this] { ChatPicker(); } },
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
          { "Commands from any draft", "Ctrl+/ or Ctrl+O · / is always literal", [this] { CloseMenu(); } },
          { "Enter sends / Shift+Enter newline", "Ctrl+J is a newline fallback", [this] { CloseMenu(); } },
          { "Escape switches focus", "Arrows browse · Enter actions", [this] { CloseMenu(); } },
          { "Ctrl+K opens Chats",
            "Open or switch a conversation; draft kept",
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
    { "Settings", "Appearance and preferences", [this] { Settings(); } },
    { "Update Harbor", "Restart when an installed update is ready", [this] { UpdatePrompt(); } },
    { "Quit Harbor", "Ctrl+Q", [this] { m_Running = false; } }
  };
  if (auto* c = Current(); c && c->back.set)
    m_Options.insert(m_Options.begin() + 1,
                     { "Return to previous position",
                       "Back from a reply, search result or pin",
                       [this]
                       {
                         CloseMenu();
                         ReturnBack();
                       } });
  auto desk = DeskOptions();
  m_Options.insert(m_Options.begin(), desk.begin(), desk.end());
  m_Dirty = true;
}
std::vector<Harbor::Option>
Harbor::DeskOptions()
{
  // Discover Desk on each menu open, including when Desk started after this Harbor window.
  // An explicit host path takes precedence over the current user's installed Desk commands.
  // Each line: label<TAB>hint<TAB>argv0<TAB>argv1... The argv is executed directly; no shell is involved.
  std::vector<Option> options;
  const char* path = getenv("HARBOR_DESK_COMMANDS");
  std::string installedPath;
  if (path == nullptr || *path == '\0')
  {
    const char* home = getenv("HOME");
    if (home == nullptr || *home == '\0')
      return options;
    installedPath = std::string(home) + "/.office/desk/harbor-desk-commands";
    path = installedPath.c_str();
  }
  std::ifstream input(path);
  std::string line;
  while (std::getline(input, line))
  {
    std::vector<std::string> fields;
    std::stringstream stream(line);
    std::string field;
    while (std::getline(stream, field, '\t'))
      fields.push_back(field);
    if (fields.size() < 3 || fields[0].empty() || fields[2].empty())
      continue;
    std::vector<std::string> argv(fields.begin() + 2, fields.end());
    const std::string shortcut = fields[0] == "Everything" ? "e · " : fields[0] == "Quota" ? "q · " : fields[0] == "Focus" ? "f · " : "";
    options.push_back({ "Desk · " + fields[0], shortcut + fields[1], [this, argv, label = fields[0]]
                        {
                          CloseMenu();
                          if (DeskHandoff(argv) && label == "Focus")
                            for (const auto& option : DeskOptions())
                              if (option.label == "Desk · Focus")
                                Notify(option.hint);
                        } });
  }
  return options;
}
void
Harbor::SuspendTerminal()
{
  HarborTrace::Event("terminal.suspend");
  SaveState();
  // As for a login UI: the child owns stdin until it ends, so the reader is stopped and joined, not left to race it.
  HarborInputStop();
  m_Media->Stop();
  m_Media->ClearGraphics();
  printf("\033[?1002l\033[<u\033[?2004l");
  fflush(stdout);
  mousemask(0, nullptr);
  curs_set(1);
  endwin();
  fflush(stdout);
}
void
Harbor::ResumeTerminal()
{
  HarborTrace::Event("terminal.resume.attempt");
  reset_prog_mode();
  if (!HarborInputStart())
    throw std::runtime_error("Could not resume terminal input reader");
  m_LastInput = HarborDeskUpdate::Now();
  mousemask(ALL_MOUSE_EVENTS | REPORT_MOUSE_POSITION, nullptr);
  printf("\033[?1003l\033[?1002h\033[?2004h\033[>1u");
  fflush(stdout);
  clearok(stdscr, TRUE);
  m_Dirty = true;
}
void
Harbor::DeskFailurePause(const std::string& reason)
{
  printf("\r\n%s · press any key to return to Harbor", reason.c_str());
  fflush(stdout);
  struct termios saved = {};
  if (tcgetattr(STDIN_FILENO, &saved) != 0)
    return;
  // A child (Node sets it on a shared tty) or curses may leave the descriptor non-blocking; the pause must block.
  const int flags = fcntl(STDIN_FILENO, F_GETFL);
  if (flags >= 0 && (flags & O_NONBLOCK))
    fcntl(STDIN_FILENO, F_SETFL, flags & ~O_NONBLOCK);
  struct termios raw = saved;
  raw.c_lflag &= ~(ICANON | ECHO);
  raw.c_cc[VMIN] = 1;
  raw.c_cc[VTIME] = 0;
  tcsetattr(STDIN_FILENO, TCSANOW, &raw);
  // Input queued before the message appeared is not an answer to it: the trailing mouse release of the click that
  // chose the entry, or typeahead sent to the child. Discard it, then wait for a new key.
  tcflush(STDIN_FILENO, TCIFLUSH);
  while (true)
  {
    unsigned char key = 0;
    const ssize_t got = read(STDIN_FILENO, &key, 1);
    if (got < 0 && errno == EINTR)
      continue;
    if (got <= 0)
      break;
    if (key != 0x1b)
      break;
    // ESC alone is a key. A mouse report (CSI < ... M/m, legacy CSI M b x y) or a focus report (CSI I/O) arriving
    // after the flush, such as a late release, is not a human key and leaves the message on screen.
    std::string seq;
    struct pollfd more = { STDIN_FILENO, POLLIN, 0 };
    while (seq.size() < 32 && poll(&more, 1, 60) > 0)
    {
      unsigned char c = 0;
      if (read(STDIN_FILENO, &c, 1) != 1)
        break;
      seq.push_back(static_cast<char>(c));
      const bool sgr = seq.size() >= 2 && seq[0] == '[' && seq[1] == '<';
      const bool legacy = seq.size() >= 2 && seq[0] == '[' && seq[1] == 'M';
      if ((sgr && (c == 'M' || c == 'm') && seq.size() > 2) || (legacy && seq.size() == 5))
        break;
      if (!sgr && !legacy && seq.size() >= 2 && c >= 0x40 && c <= 0x7e)
        break;
    }
    const bool mouse = seq.size() > 2 && seq[0] == '[' && (seq[1] == '<' || seq[1] == 'M');
    const bool focus = seq == "[I" || seq == "[O";
    if (!mouse && !focus)
      break;
  }
  tcflush(STDIN_FILENO, TCIFLUSH); // the rest of a multi-byte key is not Harbor input
  tcsetattr(STDIN_FILENO, TCSANOW, &saved);
  if (flags >= 0)
    fcntl(STDIN_FILENO, F_SETFL, flags);
}
bool
Harbor::WarmDeskHandoff(const std::vector<std::string>& argv)
{
  // Node can alter a shared descriptor while merely preparing its stream handles.
  const int flags[] = { fcntl(0, F_GETFL), fcntl(1, F_GETFL), fcntl(2, F_GETFL) };
  struct termios saved = {};
  const bool termiosSaved = tcgetattr(STDIN_FILENO, &saved) == 0;
  const auto restore = [&] {
    for (int fd = 0; fd < 3; ++fd) if (flags[fd] >= 0) fcntl(fd, F_SETFL, flags[fd]);
    if (termiosSaved) tcsetattr(STDIN_FILENO, TCSANOW, &saved);
  };
  HarborTrace::Event("desk.prepare.attempt");
  m_InDeskView = true;
  const auto prepared = m_DeskHost.Prepare(argv, [this] {
    PollUiControl();
    // Harbor still owns input during preparation. Launch typeahead cannot become a draft or Desk action.
    const auto event = HarborInputPoll(0);
    if (event.kind > 0) m_LastInput = HarborDeskUpdate::Now();
    const bool cancel = m_ControlPending || AppUtil::IsTerminateRequested() || event.kind == -1 || (event.kind == 1 && (event.code == 27 || event.code == 3 || (event.code == 'c' && (event.mods & 4))));
    free(event.text);
    return cancel;
  }, FileUtil::GetApplicationDir());
  HarborTrace::Event("desk.prepare.result", {{"outcome", prepared == HarborDeskHost::Prepared::Ready ? "ready" : prepared == HarborDeskHost::Prepared::Cancelled ? "cancelled" : prepared == HarborDeskHost::Prepared::Rejected ? "rejected" : "failed"}});
  if (prepared != HarborDeskHost::Prepared::Ready)
  {
    restore();
    m_InDeskView = false;
    if (m_ControlPending) { BeginUiRoute(); return false; }
    Notify(prepared == HarborDeskHost::Prepared::Cancelled ? "Desk opening cancelled · draft kept" : "Desk could not prepare · " + (m_DeskHost.Diagnostics().empty() ? m_DeskHost.Error() : "see <config>/" + m_DeskHost.DiagnosticName()));
    return false;
  }
  SaveState();
  HarborInputStop(); // joined before the private yield permits Office to acquire its dedicated tty reader
  m_Media->Stop(); m_Media->ClearGraphics();
  struct sigaction ignore = {}, oldInt = {}, oldQuit = {}, oldTstp = {};
  ignore.sa_handler = SIG_IGN; sigemptyset(&ignore.sa_mask);
  sigaction(SIGINT, &ignore, &oldInt); sigaction(SIGQUIT, &ignore, &oldQuit); sigaction(SIGTSTP, &ignore, &oldTstp);
  printf("\033[?1002l\033[?1003l\033[<u\033[?2004l");
  mousemask(0, nullptr); fflush(stdout);
  tcflush(STDIN_FILENO, TCIFLUSH);
  // No endwin/reset_prog_mode here: both views retain Harbor's existing alternate screen.
  HarborTrace::Event("desk.visit.attempt");
  const bool ok = m_DeskHost.Visit([] { return AppUtil::IsTerminateRequested(); }, [this] {
    PollUiControl(); return m_ControlPending;
  });
  HarborTrace::Event("desk.visit.result", {{"outcome", ok ? "released" : "failed"}});
  restore();
  tcflush(STDIN_FILENO, TCIFLUSH);
  sigaction(SIGINT, &oldInt, nullptr); sigaction(SIGQUIT, &oldQuit, nullptr); sigaction(SIGTSTP, &oldTstp, nullptr);
  if (AppUtil::IsTerminateRequested()) return false; // main loop performs ordinary state/terminal cleanup after exact child reap
  if (!HarborInputStart()) throw std::runtime_error("Could not resume terminal input reader");
  m_LastInput = HarborDeskUpdate::Now();
  mousemask(ALL_MOUSE_EVENTS | REPORT_MOUSE_POSITION, nullptr);
  printf("\033[?1003l\033[?1002h\033[?2004h\033[>1u"); fflush(stdout);
  struct winsize size = {};
  if (ioctl(STDIN_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_row && size.ws_col) resizeterm(size.ws_row, size.ws_col);
  clearok(stdscr, TRUE); m_Dirty = true; m_MouseDown = false; m_Title.clear(); // Desk may have retitled the terminal
  m_InDeskView = false;
  if (m_ControlPending) BeginUiRoute();
  else Notify(ok ? "Back from Desk" : "Desk ended unexpectedly · see <config>/" + m_DeskHost.DiagnosticName());
  return ok;
}
bool
Harbor::DeskHandoff(const std::vector<std::string>& argv)
{
  const std::string view = argv.empty() ? "other" : argv.back();
  HarborTrace::Event("desk.handoff.attempt", {{"mode", HarborDeskHost::Eligible(argv) ? "warm" : "child"},
    {"view", view == "everything" || view == "quota" || view == "focus" || view == "now" || view == "help" ? view : "other"}});
  if (HarborDeskHost::Eligible(argv)) return WarmDeskHandoff(argv);
  // The Desk view runs on this same terminal while this process, its chats, drafts and reading positions stay as they are.
  // Ctrl+C belongs to the child; Harbor ignores it until the child has ended.
  struct sigaction ignore = {}, oldInt = {}, oldQuit = {}, oldTstp = {};
  ignore.sa_handler = SIG_IGN;
  sigemptyset(&ignore.sa_mask);
  sigaction(SIGINT, &ignore, &oldInt);
  sigaction(SIGQUIT, &ignore, &oldQuit);
  sigaction(SIGTSTP, &ignore, &oldTstp);
  const int inputFlags = fcntl(STDIN_FILENO, F_GETFL);
  SuspendTerminal();
  std::vector<char*> args;
  for (const auto& arg : argv)
    args.push_back(const_cast<char*>(arg.c_str()));
  args.push_back(nullptr);
  posix_spawnattr_t attr;
  posix_spawnattr_init(&attr);
  sigset_t defaults, empty;
  sigemptyset(&defaults);
  sigaddset(&defaults, SIGINT);
  sigaddset(&defaults, SIGQUIT);
  sigaddset(&defaults, SIGTSTP);
  sigaddset(&defaults, SIGPIPE);
  sigemptyset(&empty);
  posix_spawnattr_setsigdefault(&attr, &defaults);
  posix_spawnattr_setsigmask(&attr, &empty);
  posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK);
  pid_t pid = 0;
  int spawned = posix_spawnp(&pid, args[0], nullptr, &attr, args.data(), environ);
  posix_spawnattr_destroy(&attr);
  if (spawned) HarborTrace::Os("desk.spawn.failed", spawned);
  else HarborTrace::Event("desk.spawned", {{"child_pid", HarborTrace::Number(pid)}});
  int status = 0;
  if (spawned == 0)
  {
    m_InColdDeskView = true;
    while (true)
    {
      const auto waited = waitpid(pid, &status, WNOHANG);
      if (waited == pid || (waited < 0 && errno != EINTR)) break;
      PollUiControl(); // refuse addressed control visibly while this legacy child owns input
      TimeUtil::Sleep(.02);
    }
    m_InColdDeskView = false;
  }
  HarborTrace::Event("desk.child.result", {{"spawn_error", std::to_string(spawned)}, {"wait_status", std::to_string(status)},
    {"exit_status", spawned == 0 && WIFEXITED(status) ? std::to_string(WEXITSTATUS(status)) : "unknown"},
    {"signal", spawned == 0 && WIFSIGNALED(status) ? std::to_string(WTERMSIG(status)) : "none"}});
  // The child shares this terminal's open file description; put back Harbor's own flags (a Node child sets O_NONBLOCK).
  if (inputFlags >= 0)
    fcntl(STDIN_FILENO, F_SETFL, inputFlags);
  // A failed Desk child's own error text is still on screen; keep it readable until a key, then repaint.
  // A signal (Ctrl+C) or an ordinary exit returns at once. Ctrl+C during the pause is still ignored here.
  if (spawned != 0 || (WIFEXITED(status) && WEXITSTATUS(status) != 0))
    DeskFailurePause(spawned != 0 ? "Desk could not start: " + std::string(strerror(spawned))
                                  : "Desk exited " + std::to_string(WEXITSTATUS(status)));
  sigaction(SIGINT, &oldInt, nullptr);
  sigaction(SIGQUIT, &oldQuit, nullptr);
  sigaction(SIGTSTP, &oldTstp, nullptr);
  ResumeTerminal();
  if (spawned != 0)
    Notify("Desk could not start: " + std::string(strerror(spawned)));
  else if (WIFSIGNALED(status))
    Notify("Desk interrupted (signal " + std::to_string(WTERMSIG(status)) + ") · draft kept");
  else if (WEXITSTATUS(status) != 0)
    Notify("Desk exited " + std::to_string(WEXITSTATUS(status)) + " · draft kept");
  else
    Notify("Back from Desk");
  m_Title.clear(); // the Desk view may have retitled this terminal
  return spawned == 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}
void
Harbor::Actions(bool minimal)
{
  HarborTrace::Event("ui.message_actions.open");
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
  const auto formatted = Format(*c, message);
  // the forwarded body is the decoded native text, never markup; a hidden spoiler is not forwarded
  // until the user reveals it, so neither its secret nor ▒ placeholders reach the composer
  const bool hiddenSpoiler = formatted.spoilers && !c->revealed.count(id);
  const std::wstring forwardText = formatted.native ? formatted.copy : W(message.text);
  m_Options.push_back({ "Forward",
                        "Choose a destination",
                        [this, message, key, hiddenSpoiler, forwardText]
                        {
                          if (hiddenSpoiler)
                          {
                            CloseMenu();
                            Notify("Reveal the spoiler before forwarding · nothing was forwarded");
                            return;
                          }
                          m_ForwardMessage = message;
                          m_ForwardText = forwardText;
                          m_ForwardProfile = m_Chats[key].profile;
                          m_ForwardChat = m_Chats[key].id;
                          ChatPicker(true);
                        } });
  m_Options.push_back({ "Copy text",
                        "",
                        [this, formatted]
                        {
                          CloseMenu();
                          Notify(m_Media->CopyText(S(formatted.copy)) ? "Copied" : "Could not copy");
                        } });
  if (!formatted.codes.empty())
    m_Options.push_back({ formatted.codes.size() == 1 ? "Copy code" : "Copy code…",
                          formatted.codes.size() == 1 ? "Exact contents, no markup"
                                                      : std::to_string(formatted.codes.size()) + " blocks",
                          [this, key, id] { CodeChooser(key, id); } });
  if (!message.quotedId.empty())
    m_Options.push_back({ "Go to replied message",
                          "Commands › Return goes back",
                          [this, key, message]
                          {
                            CloseMenu();
                            VisitReply(key, message);
                          } });
  if (!message.fileInfo.empty())
    m_Options.push_back({ "Open / play attachment",
                          "Images preview inline",
                          [this, key, id]
                          {
                            CloseMenu();
                            OpenAttachment(m_Chats[key].profile, m_Chats[key].id, id, true);
                          } });
  if (formatted.links.size() == 1)
  {
    std::string url = S(formatted.links.front().arg);
    m_Options.push_back({ "Open link",
                          url,
                          [this, url]
                          {
                            CloseMenu();
                            OpenLink(url);
                          } });
  }
  else if (formatted.links.size() > 1)
    m_Options.push_back({ "Open a link…",
                          std::to_string(formatted.links.size()) + " destinations",
                          [this, key, id] { LinkChooser(key, id); } });
  if (formatted.spoilers)
    m_Options.push_back({ c->revealed.count(id) ? "Hide spoiler" : "Reveal spoiler",
                          "",
                          [this, key, id]
                          {
                            auto& target = m_Chats[key];
                            if (!target.revealed.erase(id))
                              target.revealed.insert(id);
                            CloseMenu();
                          } });
  if (formatted.expandable)
    m_Options.push_back({ c->expanded.count(id) ? "Collapse quote" : "Expand quote",
                          "",
                          [this, key, id]
                          {
                            auto& target = m_Chats[key];
                            if (!target.expanded.erase(id))
                              target.expanded.insert(id);
                            CloseMenu();
                          } });
  m_Options.push_back({ "React…",
                        "Reactions this message allows",
                        [this, key, id] { ReactionChooser(key, id); } });
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
                                                        option.label == "Copy text" ||
                                                        option.label.rfind("Copy code", 0) == 0 ||
                                                        option.label == "Go to replied message";
                                   return minimal ? !primary : primary;
                                 }),
                  m_Options.end());
  if (!minimal)
    m_Menu = "message-tools";
  m_Dirty = true;
}
HarborFormatting::Text
Harbor::Format(const Conversation& c, const ChatMessage& m, bool forceConceal) const
{
  RichText::Message native;
  if (RichText::Decode(m.richText, native))
    return HarborFormatting::Native(native, !forceConceal && c.revealed.count(m.id), c.expanded.count(m.id));
  return HarborFormatting::Parse(Safe(W(m.text)));
}
std::wstring
Harbor::Excerpt(const Conversation& c, const ChatMessage& m) const
{
  // a reply excerpt never reveals a spoiler or prints a whole code block
  auto text = Format(c, m, true).copy;
  for (auto& ch : text)
    if (ch == L'\n' || ch == L'\t')
      ch = L' ';
  text = Safe(text);
  if (text.empty())
    return m.fileInfo.empty() ? L"Reply to a message" : L"Attachment";
  return text;
}
namespace
{
std::string ReplyChat(std::string chat)
{
  // Telegram message ids are unique within the physical chat, across its forum topics.
  return chat.substr(0, chat.find('#'));
}
}
std::string
Harbor::ReplyKey(const Conversation& c, const ChatMessage& m) const
{
  return Key(c.profile, ReplyChat(m.quotedChatId)) + "/" + m.quotedId;
}
const ChatMessage*
Harbor::LoadedReply(const Conversation& c, const ChatMessage& m) const
{
  if (m.quotedChatId.empty() || m.quotedChatId == "?")
    return nullptr; // legacy rows discarded the chat; never guess by a coincidentally equal id
  for (const auto& entry : m_Chats)
  {
    const auto& chat = entry.second;
    if (chat.profile != c.profile || ReplyChat(chat.id) != ReplyChat(m.quotedChatId))
      continue;
    auto it = chat.messages.find(m.quotedId);
    if (it != chat.messages.end())
      return &it->second;
  }
  return nullptr;
}
std::wstring
Harbor::ReplyExcerpt(Conversation& c, const ChatMessage& m)
{
  auto original = LoadedReply(c, m);
  auto key = ReplyKey(c, m);
  if (!original && !m.quotedChatId.empty() && m.quotedChatId != "?" && m.quotedId != "0")
  {
    auto inserted = m_ReplyOriginals.emplace(key, ReplyOriginal());
    if (inserted.second)
    {
      auto request = std::make_shared<GetMessageRequest>();
      request->chatId = m.quotedChatId;
      request->msgId = m.quotedId;
      request->replyPreview = true;
      Request(c.profile, request);
    }
    if (inserted.first->second.found)
      original = &inserted.first->second.message;
  }
  ChatMessage snippet;
  snippet.text = m.quotedText;
  snippet.richText = m.quotedRichText;
  // A supplied quote is the selected part of the original, so keep it when available.
  bool supplied = !snippet.text.empty() || !snippet.richText.empty();
  std::wstring text;
  if (supplied)
    text = Excerpt(c, snippet);
  else if (original && !original->isDeleted)
    text = Excerpt(c, *original);
  else
  {
    auto pending = m_ReplyOriginals.find(key);
    text = pending != m_ReplyOriginals.end() && pending->second.pending
             ? L"Loading original message…" : L"Original message unavailable";
  }
  std::string name = !m.quotedSenderName.empty() ? m.quotedSenderName
                     : !m.quotedSender.empty() ? Name(c.profile, m.quotedSender)
                     : original && !original->isDeleted ? Name(c.profile, original->senderId) : "";
  return name.empty() ? text : Safe(W(name)) + L": " + text;
}
void
Harbor::VisitReply(const std::string& key, const ChatMessage& m)
{
  HarborTrace::Event("ui.reply.visit");
  auto it = m_Chats.find(key);
  if (it == m_Chats.end()) return;
  auto& c = it->second;
  if (m.quotedChatId.empty() || m.quotedChatId == "?" || m.quotedId == "0")
  {
    Notify("The original message is unavailable");
    return;
  }
  const auto* loaded = LoadedReply(c, m);
  if (!loaded)
  {
    ReplyExcerpt(c, m); // starts the same deduplicated exact read if needed
    auto& pending = m_ReplyOriginals[ReplyKey(c, m)];
    if (pending.pending)
    {
      pending.visitFrom = key;
      Notify("Loading the original message…");
      return;
    }
    if (!pending.found)
    {
      Notify("The original message is unavailable");
      return;
    }
  }
  else if (loaded->isDeleted)
  {
    Notify("The original message is unavailable");
    return;
  }
  std::string target = m.quotedChatId;
  // Prefer the loaded original's topic, then the resolved Telegram topic, over the base chat.
  for (const auto& entry : m_Chats)
    if (entry.second.profile == c.profile && ReplyChat(entry.second.id) == ReplyChat(target) &&
        entry.second.messages.count(m.quotedId))
      target = entry.second.id;
  auto resolved = m_ReplyOriginals.find(ReplyKey(c, m));
  if (resolved != m_ReplyOriginals.end() && resolved->second.found)
    target = resolved->second.chat;
  auto& destination = Ensure(c.profile, target);
  const auto destinationKey = Key(destination.profile, destination.id);
  if (destinationKey != key)
  {
    Remember(c);
    // This mark returns to the source we have already reached, not an unfinished target visit.
    c.back.landed = true;
    if (resolved != m_ReplyOriginals.end() && resolved->second.found &&
        !destination.messages.count(m.quotedId))
      Merge(destination, { resolved->second.message }, true);
  }
  Visit(destinationKey, m.quotedId);
  if (destinationKey != key && destination.back.set)
    destination.back.returnKey = key;
}

void
Harbor::Remember(Conversation& c)
{
  if (c.back.set)
    return;
  c.back.set = true;
  c.back.follow = c.follow;
  c.back.browse = m_Browse;
  c.back.selected = c.selected;
  c.back.scroll = c.scroll;
  c.back.anchor.clear();
  c.back.offset = 0;
  if (!c.follow)
  {
    BuildRows();
    int row = c.scroll;
    while (row < int(m_Rows.size()) && m_Rows[row].id.empty())
      ++row;
    if (row < int(m_Rows.size()))
    {
      c.back.anchor = m_Rows[row].id;
      int first = row;
      while (first > 0 && m_Rows[first - 1].id == c.back.anchor)
        --first;
      c.back.offset = c.scroll - first;
    }
  }
}
void
Harbor::Visit(std::string key, std::string id)
{
  auto it = m_Chats.find(key);
  if (it == m_Chats.end() || id.empty())
    return;
  if (m_Current != key)
    OpenChat(key);
  auto& c = it->second;
  Remember(c);
  if (!c.messages.count(id) || c.messages[id].isDeleted)
  {
    if (c.visit == id && c.visitUntil)
      return;
    // an unloaded original: ask the cache, then Telegram; Visit runs again when it arrives
    c.visit = id;
    c.visitUntil = TimeUtil::GetCurrentTimeMSec() + 12000;
    auto request = std::make_shared<GetMessageRequest>();
    request->chatId = c.id;
    request->msgId = id;
    Request(c.profile, request);
    Notify("Loading the original message…");
    return;
  }
  c.visit.clear();
  c.visitUntil = 0;
  c.back.landed = true;
  c.restoreAnchor.clear();
  c.selected = id;
  Select(0);
  Notify("Showing the message · Ctrl+/ or Ctrl+O → Return to previous position");
}
void
Harbor::ReturnBack()
{
  HarborTrace::Event("ui.position.return");
  auto* c = Current();
  if (!c || !c->back.set)
    return;
  auto mark = c->back;
  c->back = Conversation::Mark();
  c->visit.clear();
  c->visitUntil = 0;
  if (!mark.returnKey.empty())
  {
    OpenChat(mark.returnKey);
    ReturnBack();
    return;
  }
  c->selected = mark.selected;
  m_Browse = mark.browse;
  c->follow = mark.follow;
  c->scroll = mark.scroll;
  c->restoreAnchor = mark.follow ? std::string() : mark.anchor;
  c->restoreOffset = mark.offset;
  // Keep fetched originals for reply excerpts; BuildRows hides detached rows after this return.
  m_Dirty = true;
}
// Forget out-of-sequence messages (visited originals, search hits) older than loaded history, so
// none stays drawn above the contiguous rows without a gap; one the user still refers to (selected,
// anchored, replied to or being edited) stays.
void
Harbor::DropDetached(Conversation& c)
{
  for (auto it = c.detached.begin(); it != c.detached.end();)
  {
    if (*it == c.selected || *it == c.restoreAnchor || *it == c.reply || *it == c.editing)
    {
      ++it;
      continue;
    }
    c.messages.erase(*it);
    it = c.detached.erase(it);
  }
  c.order.erase(std::remove_if(c.order.begin(),
                               c.order.end(),
                               [&c](const std::string& id) { return !c.messages.count(id); }),
                c.order.end());
}
void
Harbor::SearchPrompt()
{
  HarborTrace::Event("ui.search.open");
  m_Menu = "search";
  m_Filter.clear();
  m_Option = 0;
  m_MenuOffset = 0;
  RebuildSearch();
}
void
Harbor::RebuildSearch()
{
  m_Options.clear();
  auto query = S(m_Filter);
  if (!query.empty())
    m_Options.push_back({ "Search for “" + query + "”",
                          "On Telegram, this conversation or topic",
                          [this, query] { StartSearch(query, false); } });
  m_Options.push_back(
    { "Pinned messages", "Visit pins; nothing is pinned or unpinned", [this] { StartSearch("", true); } });
}
void
Harbor::StartSearch(const std::string& query, bool pinned)
{
  HarborTrace::Event("ui.search.attempt");
  auto* c = Current();
  if (!c)
    return;
  m_Search = Pending();
  m_Search.key = m_Current;
  m_Search.query = query;
  m_Search.pinned = pinned;
  m_Search.token = ++m_Token;
  m_Search.until = TimeUtil::GetCurrentTimeMSec() + 20000;
  auto request = std::make_shared<SearchMessagesRequest>();
  request->chatId = c->id;
  request->query = query;
  request->pinned = pinned;
  request->token = m_Search.token;
  Request(c->profile, request);
  m_Menu = "results";
  m_Filter.clear();
  m_Option = 0;
  m_MenuOffset = 0;
  m_Options = { { pinned ? "Loading pinned messages…" : "Searching Telegram…", "", [] {} } };
  m_Dirty = true;
}
void
Harbor::SearchResults(std::shared_ptr<SearchMessagesNotify> n)
{
  // only the open results view for the same profile, conversation and request uses a reply
  auto it = m_Chats.find(m_Search.key);
  const std::string ignored = !m_Search.token                  ? "no pending search"
                              : n->token != m_Search.token     ? "pending token " + std::to_string(m_Search.token)
                              : m_Menu != "results"            ? "menu " + m_Menu
                              : it == m_Chats.end()            ? "conversation gone"
                              : it->second.profile != n->profileId ? "profile " + n->profileId
                              : it->second.id != n->chatId     ? "chat " + n->chatId
                              : m_Current != m_Search.key      ? "current " + m_Current
                                                               : "";
  TestTrace("apply search token=" + std::to_string(n->token) +
            (ignored.empty() ? " accepted" : " ignored: " + ignored));
  if (!ignored.empty())
    return;
  const auto search = m_Search;
  m_Search.token = 0;
  m_Search.until = 0;
  m_Option = 0;
  m_MenuOffset = 0;
  m_Options.clear();
  auto retry = [this, search] { StartSearch(search.query, search.pinned); };
  if (!n->success)
  {
    m_Options.push_back({ "Search failed · Enter to retry",
                          n->error.empty() ? "Telegram reported an error; no result is implied" : n->error,
                          retry });
    m_Options.push_back({ "Close", "", [this] { CloseMenu(); } });
    return;
  }
  if (n->chatMessages.empty())
  {
    m_Options.push_back({ search.pinned ? "No pinned messages here" : "No messages match “" + search.query + "”",
                          "Telegram searched the whole conversation",
                          [this] { CloseMenu(); } });
    return;
  }
  auto& c = it->second;
  for (const auto& m : n->chatMessages)
  {
    const std::string key = search.key, id = m.id;
    auto label = Time(m.timeSent) + " " + (m.isOutgoing ? std::string("You") : Name(c.profile, m.senderId)) +
                 " · " + S(Excerpt(c, m));
    m_Options.push_back({ label,
                          PickerTime(m.timeSent),
                          [this, key, m]
                          {
                            CloseMenu();
                            auto& target = m_Chats[key];
                            if (!target.messages.count(m.id))
                              Merge(target, { m }, true);
                            Visit(key, m.id);
                          } });
  }
  if (n->totalCount > int32_t(n->chatMessages.size()))
    m_Options.push_back({ "Showing the newest " + std::to_string(n->chatMessages.size()) + " of " +
                            std::to_string(n->totalCount),
                          "Refine the search to see older matches",
                          [this] { CloseMenu(); } });
}
void
Harbor::ReactionChooser(const std::string& key, const std::string& id)
{
  HarborTrace::Event("ui.reaction.open");
  auto it = m_Chats.find(key);
  if (it == m_Chats.end())
    return;
  m_Reactions = Pending();
  m_Reactions.key = key;
  m_Reactions.msgId = id;
  m_Reactions.token = ++m_Token;
  m_Reactions.until = TimeUtil::GetCurrentTimeMSec() + 10000;
  auto request = std::make_shared<GetAvailableReactionsRequest>();
  request->chatId = it->second.id;
  request->msgId = id;
  Request(it->second.profile, request);
  m_Menu = "reactions";
  m_Option = 0;
  m_MenuOffset = 0;
  // opening the chooser sends nothing
  m_Options = { { "Loading reactions…", "", [] {} } };
  m_Dirty = true;
}
void
Harbor::ReactionResults(std::shared_ptr<AvailableReactionsNotify> n)
{
  auto it = m_Chats.find(m_Reactions.key);
  if (!m_Reactions.token || m_Menu != "reactions" || it == m_Chats.end() || m_Current != m_Reactions.key ||
      it->second.profile != n->profileId || it->second.id != n->chatId || m_Reactions.msgId != n->msgId)
    return;
  const std::string key = m_Reactions.key, id = m_Reactions.msgId;
  m_Reactions.token = 0;
  m_Reactions.until = 0;
  m_Option = 0;
  m_MenuOffset = 0;
  m_Options.clear();
  if (!n->success)
  {
    m_Options.push_back({ "Reactions unavailable here", "", [this] { CloseMenu(); } });
    return;
  }
  if (n->emojis.empty())
  {
    m_Options.push_back({ "No reactions allowed here", "", [this] { CloseMenu(); } });
    return;
  }
  std::string mine;
  auto message = it->second.messages.find(id);
  if (message != it->second.messages.end())
  {
    auto own = message->second.reactions.senderEmojis.find(s_ReactionsSelfId);
    if (own != message->second.reactions.senderEmojis.end())
      mine = own->second;
  }
  for (const auto& emoji : n->emojis)
    m_Options.push_back({ emoji + (emoji == mine ? "  ✓ remove" : ""),
                          "",
                          [this, key, id, emoji, mine]
                          {
                            auto& target = m_Chats[key];
                            auto r = std::make_shared<SendReactionRequest>();
                            r->chatId = target.id;
                            r->msgId = id;
                            auto message = target.messages.find(id);
                            if (message != target.messages.end())
                              r->senderId = message->second.senderId;
                            r->emoji = emoji == mine ? "" : emoji;
                            r->prevEmoji = mine;
                            Request(target.profile, r);
                            CloseMenu();
                            Notify(emoji == mine ? "Reaction removed" : "Reacted " + emoji);
                          } });
}
void
Harbor::CodeChooser(const std::string& key, const std::string& id)
{
  HarborTrace::Event("ui.code.open");
  auto& c = m_Chats[key];
  auto it = c.messages.find(id);
  if (it == c.messages.end())
    return;
  const auto codes = Format(c, it->second).codes;
  auto copy = [this](const std::wstring& content)
  {
    CloseMenu();
    Notify(m_Media->CopyText(S(content)) ? "Copied code" : "Could not copy code");
  };
  if (codes.size() == 1)
  {
    copy(codes.front().content);
    return;
  }
  m_Menu = "code";
  m_Option = 0;
  m_MenuOffset = 0;
  m_Options.clear();
  for (const auto& code : codes)
  {
    auto first = code.content.substr(0, code.content.find(L'\n'));
    size_t lines = std::count(code.content.begin(), code.content.end(), L'\n') +
                   (code.content.empty() || code.content.back() != L'\n');
    m_Options.push_back({ S(Safe(first)),
                          (code.arg.empty() ? std::string() : S(code.arg) + " · ") + std::to_string(lines) +
                            (lines == 1 ? " line" : " lines"),
                          [copy, content = code.content] { copy(content); } });
  }
}
void
Harbor::LinkChooser(const std::string& key, const std::string& id)
{
  HarborTrace::Event("ui.link.open");
  auto& c = m_Chats[key];
  auto it = c.messages.find(id);
  if (it == c.messages.end())
    return;
  const auto formatted = Format(c, it->second);
  m_Menu = "links";
  m_Option = 0;
  m_MenuOffset = 0;
  m_Options.clear();
  for (const auto& link : formatted.links)
  {
    auto label = formatted.text.substr(link.begin, link.end - link.begin);
    std::string url = S(link.arg);
    m_Options.push_back({ S(Safe(label)) + (label == link.arg ? "" : " → " + url),
                          url,
                          [this, url]
                          {
                            CloseMenu();
                            OpenLink(url);
                          } });
  }
}
void
Harbor::Timeouts(int64_t now)
{
  if (m_Search.token && m_Search.until && now > m_Search.until)
  {
    const auto search = m_Search;
    m_Search.token = 0;
    m_Search.until = 0;
    if (m_Menu == "results")
    {
      m_Option = 0;
      m_Options = { { "Search did not complete · Enter to retry",
                      "No answer from Telegram; no result is implied",
                      [this, search] { StartSearch(search.query, search.pinned); } },
                    { "Close", "", [this] { CloseMenu(); } } };
      m_Dirty = true;
    }
  }
  if (m_Reactions.token && m_Reactions.until && now > m_Reactions.until)
  {
    m_Reactions.token = 0;
    m_Reactions.until = 0;
    if (m_Menu == "reactions")
    {
      m_Option = 0;
      m_Options = { { "Reactions did not load", "Nothing was sent", [this] { CloseMenu(); } } };
      m_Dirty = true;
    }
  }
  if (auto* c = Current(); c && c->visitUntil && now > c->visitUntil)
    VisitFailed(*c);
}
// A visit that never reached its message forgets the mark Remember took for it, so a later Return
// does not jump to a position from before the failed attempt.
void
Harbor::VisitFailed(Conversation& c)
{
  c.visitUntil = 0;
  c.visit.clear();
  if (c.back.set && !c.back.landed)
    c.back = Conversation::Mark();
  Notify("The original message is unavailable");
}
void
Harbor::FilePicker(const std::string& directory)
{
  HarborTrace::Event("ui.file_picker.open");
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
  HarborTrace::Event("ui.attachments.preview");
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
  HarborTrace::Event("ui.attachments.send");
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
Harbor::OpenLink(const std::string& url)
{
  // Message destinations are data, never local paths or arbitrary opener schemes.
  std::string lower = url;
  std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
  const bool allowed = lower.rfind("https://", 0) == 0 || lower.rfind("http://", 0) == 0 ||
                       lower.rfind("tg://", 0) == 0 || lower.rfind("mailto:", 0) == 0;
  if (!allowed || std::any_of(url.begin(), url.end(), [](unsigned char c) { return c <= 32 || c == 127; }))
  {
    Notify("This link cannot be opened safely");
    return;
  }
  HarborTrace::Event("ui.link.open");
  m_Media->Open(url);
}
void
Harbor::MessageClick(int x, int y)
{
  auto* c = Current();
  if (!c)
    return;
  for (const auto& hit : m_Hits)
    if (hit.type == "attachment" && hit.y == y && x >= hit.x && x < hit.x + hit.w)
    {
      SaveAttachment(c->profile, c->id, hit.value);
      return;
    }
  // Copy cells map terminal columns (including wide glyphs and soft wraps) to the
  // formatted source. Spaces inside named links have source positions too.
  for (const auto& cell : m_CopyCells)
    if (cell.y == y && x >= cell.x && x < cell.x + cell.width)
    {
      const auto formatted = Format(*c, c->messages[cell.id]);
      if (cell.start < formatted.styles.size() &&
          (formatted.styles[cell.start] & HarborFormatting::Spoiler) && !c->revealed.count(cell.id))
        return;
      for (const auto& link : formatted.links)
        if (cell.start >= link.begin && cell.start < link.end)
        {
          OpenLink(S(link.arg));
          return;
        }
      return;
    }
}
void
Harbor::SaveAttachment(const std::string& p, const std::string& chatId, const std::string& id)
{
  auto& c = Ensure(p, chatId);
  auto it = c.messages.find(id);
  if (it == c.messages.end() || it->second.fileInfo.empty())
    return;
  auto f = ProtocolUtil::FileInfoFromHex(it->second.fileInfo);
  if (!File(f.filePath))
  {
    if (f.fileStatus == FileStatusDownloading)
    {
      Notify("Attachment is downloading…");
      return;
    }
    auto request = std::make_shared<DownloadFileRequest>();
    request->chatId = chatId;
    request->msgId = id;
    request->fileId = f.fileId;
    request->downloadFileAction = DownloadFileActionSave;
    f.fileStatus = FileStatusDownloading;
    it->second.fileInfo = ProtocolUtil::FileInfoToHex(f);
    Request(p, request);
    Notify("Downloading attachment to Downloads…");
    return;
  }
  // The UI is a separate process from the backend. Read this one preference
  // without initializing or saving the backend-owned AppConfig (or its credentials).
  std::ifstream config(FileUtil::GetApplicationDir() + "/app.conf");
  std::string line, downloads;
  while (std::getline(config, line))
  {
    const auto separator = line.find('=');
    if (separator == std::string::npos)
      continue;
    auto key = line.substr(0, separator);
    StrUtil::Trim(key);
    if (key == "downloads_dir")
    {
      downloads = line.substr(separator + 1);
      StrUtil::Trim(downloads);
    }
  }
  FileUtil::SetDownloadsDir(downloads);
  const auto directory = FileUtil::GetDownloadsDir();
  const auto name = FileUtil::BaseName(f.filePath);
  std::string destination;
  int output = -1;
  for (int suffix = 0; suffix < 10000; ++suffix)
  {
    destination = directory + "/" + (suffix ? FileUtil::RemoveFileExt(name) + "_" +
                       std::to_string(suffix) + FileUtil::GetFileExt(name) : name);
    output = open(destination.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (output >= 0 || errno != EEXIST)
      break;
  }
  std::ifstream input(f.filePath, std::ios::binary);
  bool ok = output >= 0 && input.good();
  char buffer[65536];
  while (ok && input)
  {
    input.read(buffer, sizeof(buffer));
    auto count = input.gcount();
    for (std::streamsize offset = 0; ok && offset < count;)
    {
      auto written = write(output, buffer + offset, size_t(count - offset));
      if (written < 0 && errno == EINTR)
        continue;
      if (written <= 0)
        ok = false;
      else
        offset += written;
    }
  }
  ok = ok && !input.bad();
  if (output >= 0)
  {
    if (close(output) != 0)
      ok = false;
    if (!ok)
      unlink(destination.c_str());
  }
  HarborTrace::Event(ok ? "ui.attachment.saved" : "ui.attachment.save_failed");
  Notify(ok ? "Saved to " + destination : "Could not save attachment to " + directory);
}
void
Harbor::OpenAttachment(const std::string& p, const std::string& chatId, const std::string& id, bool preview)
{
  HarborTrace::Event("ui.attachment.open");
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
    Notify("Voice playback · Ctrl+/ to pause or stop");
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
  HarborTrace::Event("ui.clipboard.paste");
  std::string image = m_Media->PasteboardImage();
  if (!image.empty())
    AttachPreview({ image });
  else
    Paste(W(Clipboard::GetText()));
}
void
Harbor::DrawMenu()
{
  // Flatten the conversation into a quiet backdrop before painting the dialog.
  // Keep the text in place so closing the dialog does not change reading position.
  for (int row = m_DeskStrip.Enabled() ? 4 : 0; row < m_H; ++row)
    mvchgat(row, 0, m_W, A_NORMAL, Backdrop, nullptr);
  bool picker = m_Menu == "chats" || m_Menu == "forward";
  bool compact = m_Menu == "actions" || m_Menu == "message-tools" || m_Menu == "update" ||
                 m_Menu == "reactions" || picker;
  int width = std::min(m_MainW - 4, picker ? 64 : compact ? 38 : 64);
  int x = m_MainX + (m_MainW - width) / 2;
  int y =
    4 + (m_AnimatedUntil ? std::max(0, int((m_AnimatedUntil - TimeUtil::GetCurrentTimeMSec()) / 40)) : 0);
  int stride = compact ? 1 : 2;
  int maxItems = std::max(1, std::min(picker ? 8 : 1000, (m_H - 10) / stride));
  int extra = (m_Menu == "attachments" || m_Menu == "preview") ? std::min(12, m_H / 3) : 0;
  int captionHeight = m_Menu == "attachments" ? 4 : 0;
  int optionsHeight = std::min(maxItems, int(m_Options.size())) * stride;
  int height = std::min(m_H - 5, (picker ? 8 : 5) + optionsHeight + extra + captionHeight + 2);
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
    Text(row, x, width, L"", Dialog);

  Text(y, x, width, L"╭" + std::wstring(width - 2, L'─') + L"╮", DialogBorder);
  Text(y + height - 1, x, width, L"╰" + std::wstring(width - 2, L'─') + L"╯", DialogBorder);
  for (int row = y + 1; row < y + height - 1; ++row)
  {
    Text(row, x, 1, L"│", DialogBorder);
    Text(row, x + width - 1, 1, L"│", DialogBorder);
  }

  std::string title = m_Menu == "settings"        ? "Settings"
                      : m_Menu == "appearance"      ? "Appearance"
                      : m_Menu == "chats"           ? "Find a conversation"
                      : m_Menu == "forward"       ? "Forward to…"
                      : m_Menu == "actions"       ? "Message actions"
                      : m_Menu == "message-tools" ? "Message tools"
                      : m_Menu == "update"        ? "Update ready"
                      : m_Menu == "commands"      ? "Commands & shortcuts"
                      : m_Menu == "files"         ? "Attach · " + m_Directory
                      : m_Menu == "delete"        ? "Delete message?"
                      : m_Menu == "help"          ? "Keyboard help"
                      : m_Menu == "preview"       ? "Image preview"
                      : m_Menu == "search"        ? "Search this conversation"
                      : m_Menu == "results"       ? (m_Search.pinned ? "Pinned messages" : "Search results")
                      : m_Menu == "reactions"     ? "React"
                      : m_Menu == "code"          ? "Copy code"
                      : m_Menu == "links"         ? "Open a link"
                                                  : "Preview attachments";
  Text(y + 1, x + 2, width - 4, W(title), DialogBorder, true);
  int start = y + (picker ? 5 : 3);
  bool filtering = m_Menu == "chats" || m_Menu == "forward" || m_Menu == "files" || m_Menu == "search";
  if (filtering)
    Text(y + (picker ? 3 : 2),
         x + 2,
         width - 4,
         L"› " + m_Filter + (m_Filter.empty() ? L"search…" : L""),
         Selected);
  if (extra && !m_Attachments.empty())
  {
    auto& file = m_Attachments.front();
    Text(start, x + 2, width - 4, W(FileUtil::BaseName(file) + " · " + Size(file)), Dialog);
    int rows = std::max(1, extra - 2);
    int cols = width - 4;
    m_Media->RequestImage(file, cols, rows, Type(file).find("video") == 0);
    auto* image = m_Media->Image(file, cols, rows);
    if (image && !image->failed)
      m_Media->DrawImage(*image, start + 1, x + 2, cols, rows);
    else
      Text(start + 2, x + 2, width - 4, W(Type(file)), Dialog);
    start += extra;
  }
  int captionY = -1;
  if (captionHeight)
  {
    captionY = start;
    Text(start, x + 2, width - 4, L"Caption · type to add", Dialog);
    auto* c = Current();
    auto layout = Layout(c ? c->draft : L"", std::max(1, width - 6));
    auto point = CursorPoint(layout, c ? std::min(c->cursor, c->draft.size()) : 0,
                             c && c->cursorAtRowEnd);
    int first = std::max(0, point.row - 1);
    for (int i = 0; i < 2; ++i)
    {
      m_Hits.push_back({ start + 1 + i, x + 2, width - 4, "caption", std::to_string(first + i) });
      Text(start + 1 + i,
           x + 2,
           width - 4,
           L" " + (first + i < int(layout.lines.size()) ? layout.lines[first + i] : L""),
           Selected);
    }
    start += captionHeight;
  }
  m_OptionY = start;
  int row = start;
  for (int i = m_MenuOffset;
       i < int(m_Options.size()) && row < y + height - (picker ? 3 : 2) && i < m_MenuOffset + maxItems;
       i++, row += stride)
  {
    // Titles are always bold; descriptions are muted and indented beneath them.
    // The selected option fills the dialog's inner width on every row it occupies
    // and carries a selection arrow, which stays visible when colors fall back
    // to the terminal default.
    auto label = W(m_Options[i].label);
    const bool selected = i == m_Option;
    const int color = selected ? Selected : Dialog;
    const int quiet = selected ? SelectedMuted : DialogMuted;
    for (int line = row; line < row + stride; ++line)
    {
      Text(line, x + 1, width - 2, L"", color);
      if (selected && !picker && line == row)
        Text(line, x + 1, 1, L"›", LinkSelected, true);
    }
    if (picker)
    {
      label = (selected ? L"› " : L"  ") + label;
      const auto right = W((m_Options[i].hint.empty() ? "  " : "● ") + m_Options[i].time);
      int reserve = std::max(7, Width(right));
      int room = std::max(1, width - 6 - reserve);
      Text(row, x + 2, room, label, color, true);
      if (width >= 60 && room - Width(label) >= 14 && !m_Options[i].preview.empty())
        Text(row, x + 4 + Width(label), room - Width(label) - 2, W(m_Options[i].preview), quiet);
      Text(row, x + width - 2 - reserve, reserve, right, quiet);
    }
    else
      Text(row, x + 2, width - 4, label, color, true);
    if (!compact)
      Text(row + 1, x + 4, width - 6, W(m_Options[i].hint), quiet);
    m_Hits.push_back({ row, x + 2, width - 4, "option", std::to_string(i) });
  }
  Text(y + height - 2,
       x + 2,
       width - 4,
       m_Options.empty() ? L"No matches · Esc back" : L"↑↓ · Enter select · Esc close",
       Dialog);
  if (captionY >= 0)
  {
    auto* c = Current();
    auto layout = Layout(c ? c->draft : L"", std::max(1, width - 6));
    auto point = CursorPoint(layout, c ? std::min(c->cursor, c->draft.size()) : 0,
                             c && c->cursorAtRowEnd);
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
      if (m_Menu == "commands" && (k == 'e' || k == 'q' || k == 'f'))
      {
        const std::string label = k == 'e' ? "Desk · Everything" : k == 'q' ? "Desk · Quota" : "Desk · Focus";
        for (size_t i = 0; i < m_Options.size(); ++i)
          if (m_Options[i].label == label)
          {
            Choose(int(i));
            return;
          }
      }
      if (m_Menu == "attachments")
      {
        Insert(std::wstring(1, wchar_t(k)));
        return;
      }
      if (m_Menu == "chats" || m_Menu == "forward" || m_Menu == "files" || m_Menu == "search")
      {
        m_Filter += wchar_t(k);
        m_Option = 0;
        m_MenuOffset = 0;
        if (m_Menu == "files")
          RebuildFiles();
        else if (m_Menu == "search")
          RebuildSearch();
        else
          RebuildPicker(false);
      }
      return;
    }
    auto* c = Current();
    if (!c)
      return;
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
    PreserveReadingPosition();
    clearok(stdscr, TRUE);
    return;
  }
  if (k == 17)
  {
    m_Running = false;
    return;
  }
  // Ctrl+/ and Ctrl+O open Commands from any draft without changing it; plain / is always literal.
  if (k == 15 && m_Menu.empty())
  {
    Commands();
    return;
  }
  if (k == 7)
  {
    // Latest must not discard a pending attachment's confirmation and caption.
    if (m_Menu != "attachments")
      CloseMenu();
    Latest();
    return;
  }
  if (k == 18)
  {
    if (!m_InitialChat.empty())
    {
      m_InitialRouteUntil = TimeUtil::GetCurrentTimeMSec() + 30000;
      m_InitialRouteStatus = "Opening requested conversation…";
      auto request = std::make_shared<GetChatsRequest>();
      request->includeForumTopics = true;
      Request(m_InitialProfile, request);
      return;
    }
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
         k == NewlineKey || k == 10 || k == BackwardWordKey || k == ForwardWordKey))
    {
      EditKey(k);
      return;
    }
    // Composer-only commands must not become Unicode text in picker filters.
    if (k == BackwardWordKey || k == ForwardWordKey)
      return;
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
    if (m_Menu == "chats" || m_Menu == "forward" || m_Menu == "files" || m_Menu == "search")
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
      else if (m_Menu == "search")
        RebuildSearch();
      else
        RebuildPicker(false);
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
  if (k == NewlineKey || k == 10 || k == DeleteWordKey || k == 23 ||
      k == BackwardWordKey || k == ForwardWordKey)
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
      EditKey(k);
    }
    return;
  }
  if (k == 13 || k == KEY_ENTER)
  {
    Send();
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
  HarborTrace::Event("ui.selection.copy");
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
    auto decor = m_CopyDecor.find(id);
    for (size_t i = start; i < end; ++i)
      if (decor == m_CopyDecor.end() || i >= decor->second.size() || !decor->second[i])
        copied += text->second[i];
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
Harbor::PreserveReadingPosition()
{
    // Rewrapping earlier messages must not change the row the reader is looking at.
    if (auto* c = Current(); c && !c->follow && c->scroll < int(m_Rows.size()))
    {
      int row = c->scroll;
      while (row < int(m_Rows.size()) && m_Rows[row].id.empty()) ++row;
      if (row < int(m_Rows.size()))
      {
        c->restoreAnchor = m_Rows[row].id;
        int first = row;
        while (first > 0 && m_Rows[first - 1].id == c->restoreAnchor) --first;
        c->restoreOffset = c->scroll - first;
      }
    }
}
void
Harbor::ReadInput()
{
  struct winsize size = {};
  if (ioctl(STDIN_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_row && size.ws_col &&
      (size.ws_row != m_H || size.ws_col != m_W))
  {
    PreserveReadingPosition();
    resizeterm(size.ws_row, size.ws_col);
    clearok(stdscr, TRUE);
    m_Dirty = true;
  }
  auto event = HarborInputPoll(20);
  if (event.kind > 0) m_LastInput = HarborDeskUpdate::Now();
  if (event.kind == -1)
    m_Running = false;
  else if (event.kind == 2)
  {
    m_KeyIsSpecial = false;
    for (auto ch : W(event.text ? event.text : ""))
      KeyPress(ch);
  }
  else if (event.kind == 3)
    Paste(W(event.text ? event.text : ""));
  else if (event.kind == 4)
  {
    MEVENT mouse = {};
    mouse.x = event.x;
    mouse.y = event.y;
    if (event.button == 1 || (event.button == 0 && event.action == 2 && m_MouseDown))
      mouse.bstate = event.action == 2 ? BUTTON1_RELEASED : BUTTON1_PRESSED;
    else if (event.button == 3)
      mouse.bstate = event.action == 2 ? BUTTON3_RELEASED : BUTTON3_PRESSED;
    else if (event.button == 4)
      mouse.bstate = BUTTON4_PRESSED;
    if (event.action == 3)
      mouse.bstate = REPORT_MOUSE_POSITION;
    // The decoder supplies wheel-down even when the selected ncurses headers
    // have no BUTTON5_PRESSED. Keep that typed event outside ncurses' mouse ABI.
    Mouse(mouse, event.button == 5 && event.action == 1, std::max(1, event.code));
  }
  else if (event.kind == 1)
  {
    const int keys[] = { KEY_LEFT, KEY_RIGHT, KEY_UP, KEY_DOWN, KEY_HOME,
                         KEY_END, KEY_PPAGE, KEY_NPAGE, KEY_DC, KEY_IC };
    wint_t key = event.code;
    m_KeyIsSpecial = event.code < 0;
    if (m_KeyIsSpecial)
    {
      key = keys[-event.code - 1];
      if (event.mods == 2 && (event.code == -1 || event.code == -2))
        key = event.code == -1 ? BackwardWordKey : ForwardWordKey;
    }
    // Some terminals send Option-arrows as legacy Escape-b / Escape-f.
    else if (event.mods == 2 && (event.code == 'b' || event.code == 'f'))
    {
      key = event.code == 'b' ? BackwardWordKey : ForwardWordKey;
      m_KeyIsSpecial = true;
    }
    // Legacy BS is represented by the decoder as Ctrl+H. Preserve the same
    // word-delete binding for Alt+Backspace on terminals that send BS.
    else if (event.code == 'h' && (event.mods & 6) == 6)
    {
      key = DeleteWordKey;
      m_KeyIsSpecial = true;
    }
    // Enhanced Super+Backspace (Cmd on macOS) shares Ctrl-U's line delete;
    // legacy terminals already send Ctrl-U for Cmd-Backspace.
    else if ((event.code == 127 || event.code == 8) && event.mods == 32)
      key = 21;
    else if (event.code == 127 || event.code == 8)
    {
      key = event.mods & 2 ? DeleteWordKey : KEY_BACKSPACE;
      m_KeyIsSpecial = true;
    }
    else if (event.code == 13 && event.mods & 3)
    {
      key = NewlineKey;
      m_KeyIsSpecial = true;
    }
    else if ((event.mods & 2) && (event.code == 'k' || event.code == 'K'))
    {
      key = FindKey;
      m_KeyIsSpecial = true;
    }
    // Exact decoded Super-O (32) opens Commands; existing Ctrl combinations keep their previous behavior.
    else if (event.mods == 32 && event.code == 'o')
      key = 15;
    // Legacy Ctrl+/ is C0 US (Ctrl+_), decoded as underscore+Ctrl.
    // Enhanced terminals can report slash or underscore with the exact Ctrl modifier.
    else if ((event.mods == 4 && (event.code == '/' || event.code == '_' || event.code == 31)) ||
             (event.mods == 0 && event.code == 31))
      key = 15;
    // Retain the existing Option-/ alias; plain / is always text.
    else if (event.mods == 2 && event.code == '/')
      key = 15;
    else if ((event.mods & 4) && event.code >= 'a' && event.code <= 'z')
      key = event.code - 'a' + 1;
    else if (event.mods & (2 | 4 | 8 | 32))
    {
      free(event.text);
      return; // Unbound modified keys are commands, never draft text.
    }
    KeyPress(key);
  }
  free(event.text);
}

void
Harbor::Mouse(const MEVENT& e, bool wheelDown, int wheelTicks)
{
  m_Dirty = true;

  auto* c = Current();
  auto placeCursor = [&](const TextLayout& layout, int row, int col)
  {
    const size_t start = layout.rowStarts[row], end = layout.rowEnds[row];
    size_t best = end;
    int distance = std::abs(Width(layout.lines[row]) - col);
    // Clicking the last glyph, or the blank cells after it, places after it.
    size_t last = end;
    while (last > start && Width(c->draft[last - 1]) == 0)
      --last;
    if (last > start && col < layout.cursors[last - 1].col)
      for (size_t i = start; i < end; ++i)
        if (Width(c->draft[i]) > 0)
        {
          int delta = std::abs(layout.cursors[i].col - col);
          if (delta < distance)
          {
            distance = delta;
            best = i;
          }
        }
    c->cursor = best;
    c->cursorAtRowEnd = best == end;
    c->selected.clear();
    m_Browse = m_MouseDown = m_MouseDragging = m_HasSelection = false;
  };
  if (c && (e.bstate & BUTTON1_PRESSED))
  {
    if (m_Menu.empty() && e.y >= m_ComposerY + 1 &&
        e.y < m_ComposerY + 1 + m_ComposerRows && e.x >= m_MainX + 2 && e.x < m_W - 2)
    {
      auto layout = Layout(c->draft, std::max(1, m_MainW - 11));
      int row = std::min(int(layout.lines.size()) - 1, m_ComposerFirstLine + e.y - m_ComposerY - 1);
      placeCursor(layout, row, std::max(0, e.x - m_MainX - 6));
      return;
    }
    if (m_Menu == "attachments")
      for (const auto& hit : m_Hits)
        if (hit.type == "caption" && e.y == hit.y && e.x >= hit.x && e.x < hit.x + hit.w)
        {
          auto layout = Layout(c->draft, std::max(1, m_DialogW - 6));
          int row = std::min(int(layout.lines.size()) - 1, std::stoi(hit.value));
          placeCursor(layout, row, std::max(0, e.x - m_DialogX - 3));
          return;
        }
  }
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
    if (wheelDown || (e.bstate & BUTTON4_PRESSED))
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
        MessageClick(e.x, e.y);
        return;
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
          m_Browse = false;
          if (c)
            c->selected.clear();
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
      // The visible Latest bar changes only the history, keeping attachment confirmation open.
      if (m_Menu == "attachments")
        for (const auto& hit : m_Hits)
          if (hit.type == "latest" && e.y == hit.y && e.x >= hit.x && e.x < hit.x + hit.w)
          {
            Latest();
            return;
          }
      m_Browse = false;
      if (c)
        c->selected.clear();
      CloseMenu();
      m_Dirty = true;
      return;
    }
    if (e.bstate & BUTTON4_PRESSED)
    {
      m_Option = std::max(0, m_Option - wheelTicks);
      return;
    }
    if (wheelDown)
    {
      m_Option = std::min(int(m_Options.size()) - 1, m_Option + wheelTicks);
      return;
    }
  }
  if (e.bstate & BUTTON4_PRESSED)
  {
    if (c)
    {
      c->follow = false;
      c->scroll = std::max(0, c->scroll - 3 * wheelTicks);
      if (c->scroll < 5)
        Fetch(*c, true);
    }
    return;
  }
  if (wheelDown)
  {
    if (c)
    {
      c->follow = false;
      const int last = std::max(0, int(m_Rows.size()) - std::max(1, m_Bottom - m_Top));
      c->scroll = std::min(last, c->scroll + 3 * wheelTicks);
    }
    return;
  }
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
    else if ((hit.type == "message" || hit.type == "attachment") && c)
    {
      MessageClick(e.x, e.y);
      m_Browse = false;
      c->selected.clear();
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
  if (!HarborInputStart())
  {
    endwin();
    return;
  }
  printw("Harbor input inspector · Ctrl+Q exits\n");
  refresh();
  while (true)
  {
    auto event = HarborInputPoll(1000);
    bool quit = event.kind == -1 ||
                (event.kind == 1 && (event.code == 17 || (event.code == 'q' && (event.mods & 4))));
    if (!quit && event.kind)
    {
      printw("kind=%d code=%d mods=%d x=%d y=%d button=%d action=%d\n",
             event.kind, event.code, event.mods, event.x, event.y, event.button, event.action);
      refresh();
    }
    free(event.text);
    if (quit)
      break;
  }
  HarborInputStop();
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
Ui::SetSession(int p_Slot, std::function<void()> p_OnAuthSuspended)
{
  m_Harbor->SetSession(p_Slot, p_OnAuthSuspended);
}
void
Ui::SetInitialChat(const std::string& profile, const std::string& conversation)
{
  m_Harbor->SetInitialChat(profile, conversation);
}
void
Ui::RunKeyDump()
{
  Harbor::KeyDump();
}
