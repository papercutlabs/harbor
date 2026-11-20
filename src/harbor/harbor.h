#pragma once
#include "format.h"
#include "typing.h"
#include "deskhost.h"
#include "deskstrip.h"
#include "deskupdate.h"
#include "uicontrol.h"
#include "protocol.h"
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <ncurses.h>
#include <set>
#include <string>
#include <termios.h>
#include <vector>

class HarborMedia;
class Harbor
{
public:
  Harbor();
  ~Harbor();
  void Init();
  void Cleanup();
  void Run();
  void AddProtocol(std::shared_ptr<Protocol>);
  std::unordered_map<std::string, std::shared_ptr<Protocol>> Protocols();
  void Receive(std::shared_ptr<ServiceMessage>);
  static void KeyDump();
  // Shared-session window: per-view state slot (0 keeps the existing harbor-state file) and the
  // hook that hands the terminal to the session backend once an auth prompt has suspended ncurses.
  void SetSession(int p_Slot, std::function<void()> p_OnAuthSuspended);
  void SetInitialChat(const std::string& p_Profile, const std::string& p_Conversation);

private:
  std::string StatePath() const;
  std::string LegacyStatePath() const;
  std::string StableStatePath() const;
  void TryDeskUpdate();
  int64_t m_LastInput = HarborDeskUpdate::Now();
  bool m_StableStateUsable = true;
  struct termios m_InitialTerminalMode = {};
  bool m_HaveInitialTerminalMode = false;
  bool m_Remote = false;
  int m_Slot = 0;
  std::function<void()> m_OnAuthSuspended;
  struct Conversation
  {
    std::string profile, id, name;
    HarborTyping typing;
    std::map<std::string, ChatMessage> messages;
    std::vector<std::string> order;
    std::wstring draft;
    size_t cursor = 0;
    bool cursorAtRowEnd = false; // transient visual affinity at a soft-wrap boundary
    std::wstring draftBeforeEdit;
    size_t cursorBeforeEdit = 0;
    std::string replyBeforeEdit;
    std::string selected, reply, editing, marked;
    std::vector<ChatMessage> recovery;
    std::string restoreAnchor;
    int restoreOffset = 0;
    int scroll = 0;
    int unseen = 0;
    int historyStalls = 0;
    int64_t historyRetryAt = 0;
    bool follow = true, loaded = false, more = true, fetching = false, unread = false;
    int64_t last = 0, actualLast = 0;
    std::string preview;
    bool previewNative = false; // preview came from native text: show it literally, never parse it
    // per-message presentation the user chose: revealed spoilers, expanded quotes
    std::set<std::string> revealed, expanded;
    // messages fetched out of sequence (reply originals, search hits) older than loaded history;
    // history paging continues from the oldest contiguous message instead
    std::set<std::string> detached;
    // reading position to return to after visiting a reply original, search result or pin
    struct Mark
    {
      bool set = false, follow = true, browse = false;
      bool landed = false; // a visit reached its message; until then a failed visit forgets the mark
      std::string selected, anchor, returnKey;
      int scroll = 0, offset = 0;
    } back;
    std::string visit; // message to show once it arrives
    int64_t visitUntil = 0;
  };
  // An asynchronous chooser request bound to its profile, conversation and message; replies that
  // do not match the open chooser are ignored.
  struct Pending
  {
    std::string key, msgId, query;
    bool pinned = false;
    int64_t token = 0, until = 0;
  };
  struct Row
  {
    std::wstring text;
    std::string id;
    bool sent = false, header = false;
    std::vector<unsigned> styles = {};
    std::vector<size_t> sources = {};
  };
  struct Option
  {
    std::string label, hint;
    std::function<void()> run;
    std::string key = {}, time = {}, preview = {};
  };
  struct Hit
  {
    int y, x, w;
    std::string type, value;
  };
  std::mutex m_Mutex;
  std::deque<std::shared_ptr<ServiceMessage>> m_Events;
  std::unordered_map<std::string, std::shared_ptr<Protocol>> m_Protocols;
  std::map<std::string, std::map<std::string, ContactInfo>> m_Contacts;
  std::map<std::string, Conversation> m_Chats, m_SavedStates;
  std::string m_LastState, m_RestoreChat;
  std::string m_InitialChat, m_InitialProfile, m_InitialRouteStatus;
  int64_t m_InitialRouteUntil = 0;
  HarborUiControl::Server m_UiControl;
  int m_ControlReply = -1;
  std::string m_ControlProfile, m_ControlConversation, m_ControlKey, m_ControlError;
  bool m_ControlPending = false, m_InDeskView = false, m_InColdDeskView = false;
  void PollUiControl();
  void BeginUiRoute();
  void FinishUiRoute(const std::string& error = {});
  std::set<std::string> m_Connected;
  std::vector<std::string> m_ChatOrder;
  std::string m_Title;
  std::string TerminalTitle(const std::string&);
  void SetTitle(const std::string&);
  std::string m_Current, m_Menu, m_Status, m_AuthOwner, m_ForwardProfile, m_ForwardChat;
  std::wstring m_Filter;
  std::vector<Option> m_Options;
  int m_Option = 0, m_MenuOffset = 0;
  std::vector<std::string> m_Attachments;
  std::string m_Directory;
  std::string m_UpdateTarget, m_UpdateVersion;
  int64_t m_LastUpdateCheck = 0;
  void CheckUpdate();
  void UpdatePrompt();
  ChatMessage m_ForwardMessage;
  std::wstring m_ForwardText;
  struct ReplyOriginal
  {
    bool pending = true, found = false;
    ChatMessage message;
    std::string chat, visitFrom;
  };
  std::map<std::string, ReplyOriginal> m_ReplyOriginals;
  std::string ReplyKey(const Conversation&, const ChatMessage&) const;
  const ChatMessage* LoadedReply(const Conversation&, const ChatMessage&) const;
  std::wstring ReplyExcerpt(Conversation&, const ChatMessage&);
  void VisitReply(const std::string&, const ChatMessage&);
  std::vector<Row> m_Rows;
  std::vector<Hit> m_Hits;
  struct CopyCell
  {
    int y, x, width;
    std::string id;
    size_t start, end;
  };
  std::vector<CopyCell> m_CopyCells;
  std::map<std::string, std::wstring> m_CopyTexts;
  std::map<std::string, std::vector<bool>> m_CopyDecor;
  bool m_MouseDown = false, m_MouseDragging = false, m_HasSelection = false;
  int64_t m_MouseActivityAt = 0;
  int m_SelectionX = 0, m_SelectionY = 0, m_SelectionEndX = 0, m_SelectionEndY = 0;
  bool CellSelected(const CopyCell&) const;
  void CopySelection();
  std::unique_ptr<HarborMedia> m_Media;
  HarborDeskStrip m_DeskStrip;
  bool m_Running = true, m_Ready = false, m_Dirty = true, m_Browse = false,
       m_Motion = true, m_KeyIsSpecial = false;
  int m_TypingPhase = -1;
  int64_t m_StatusUntil = 0, m_LastDraw = 0, m_AnimatedUntil = 0;
  int m_H = 0, m_W = 0, m_MainX = 0, m_MainW = 0, m_Top = 5, m_Bottom = 0, m_ComposerY = 0,
      m_ComposerRows = 1;
  int m_DialogY = 0, m_DialogX = 0, m_DialogW = 0, m_DialogH = 0, m_OptionY = 0, m_ComposerFirstLine = 0;
  bool m_AuthSuspended = false;
  std::string Key(const std::string&, const std::string&) const;
  Conversation* Current();
  Conversation& Ensure(const std::string&, const std::string&);
  std::string Name(const std::string&, const std::string&) const;
  void Request(const std::string&, std::shared_ptr<RequestMessage>);
  void LoadState();
  bool SaveState(bool force = false);
  void ProcessEvents();
  void Event(std::shared_ptr<ServiceMessage>);
  void SortChats();
  void Merge(Conversation&, const std::vector<ChatMessage>&, bool outOfSequence = false);
  void OpenChat(const std::string&);
  void Fetch(Conversation&, bool older = false, bool automatic = false);
  void Send();
  void BeginEdit(Conversation&, const std::string&);
  void EndEdit(Conversation&);
  void Select(int);
  void Latest();
  void Notify(const std::string&);
  void Draw();
  void DrawMenu();
  void BuildRows();
  void Text(int, int, int, const std::wstring&, int color, bool bold = false);
  void Fill(int, int, int, int color);
  void KeyPress(wint_t);
  void Mouse(const MEVENT& e, bool wheelDown = false, int wheelTicks = 1);
  void ReadInput();
  void EditKey(wint_t);
  void Insert(const std::wstring&);
  void Paste(const std::wstring&);
  void ChatPicker(bool forward = false);
  void RebuildPicker(bool preserveSelection = true);
  void Commands();
  // Optional Desk entries named by HARBOR_DESK_COMMANDS, each run as an argv child on this same terminal.
  std::vector<Option> DeskOptions();
  HarborDeskHost m_DeskHost;
  bool WarmDeskHandoff(const std::vector<std::string>& argv);
  bool DeskHandoff(const std::vector<std::string>& argv);
  void DeskFailurePause(const std::string& reason);
  void SuspendTerminal();
  void ResumeTerminal();
  void Actions(bool minimal = true);
  void CloseMenu();
  void Choose(int);
  void FilePicker(const std::string&);
  void RebuildFiles();
  void AttachPreview(std::vector<std::string>);
  void SendAttachments();
  void OpenAttachment(const std::string&, const std::string&, const std::string&, bool preview = false);
  void ClipboardPaste();
  HarborFormatting::Text Format(const Conversation&, const ChatMessage&, bool forceConceal = false) const;
  std::wstring Excerpt(const Conversation&, const ChatMessage&) const;
  void Remember(Conversation&);
  void Visit(std::string key, std::string id); // by value: callers pass members Visit clears
  void ReturnBack();
  void SearchPrompt();
  void RebuildSearch();
  void DropDetached(Conversation&);
  void VisitFailed(Conversation&);
  void StartSearch(const std::string& query, bool pinned);
  void SearchResults(std::shared_ptr<SearchMessagesNotify>);
  void ReactionChooser(const std::string& key, const std::string& id);
  void ReactionResults(std::shared_ptr<AvailableReactionsNotify>);
  void CodeChooser(const std::string& key, const std::string& id);
  void LinkChooser(const std::string& key, const std::string& id);
  void Timeouts(int64_t now);
  Pending m_Search, m_Reactions;
  int64_t m_Token = 0;
};
