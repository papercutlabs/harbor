#pragma once
#include "protocol.h"
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <ncurses.h>
#include <set>
#include <string>
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

private:
  struct Conversation
  {
    std::string profile, id, name;
    std::map<std::string, ChatMessage> messages;
    std::vector<std::string> order;
    std::wstring draft;
    size_t cursor = 0;
    std::wstring draftBeforeEdit;
    size_t cursorBeforeEdit = 0;
    std::string replyBeforeEdit;
    std::string selected, reply, editing, marked;
    std::vector<ChatMessage> recovery;
    int scroll = 0;
    int unseen = 0;
    int historyStalls = 0;
    int64_t historyRetryAt = 0;
    bool follow = true, loaded = false, more = true, fetching = false, unread = false;
    int64_t last = 0, actualLast = 0;
    std::string preview;
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
  std::string m_LastState;
  std::set<std::string> m_Connected;
  std::vector<std::string> m_ChatOrder;
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
  bool m_MouseDown = false, m_MouseDragging = false, m_HasSelection = false;
  int64_t m_MouseActivityAt = 0;
  int m_SelectionX = 0, m_SelectionY = 0, m_SelectionEndX = 0, m_SelectionEndY = 0;
  bool CellSelected(const CopyCell&) const;
  void CopySelection();
  std::unique_ptr<HarborMedia> m_Media;
  bool m_Running = true, m_Ready = false, m_Dirty = true, m_Browse = false, m_Pasting = false,
       m_Motion = true, m_KeyIsSpecial = false;
  std::wstring m_Paste;
  int64_t m_PasteStart = 0, m_StatusUntil = 0, m_LastDraw = 0, m_AnimatedUntil = 0;
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
  void SaveState();
  void ProcessEvents();
  void Event(std::shared_ptr<ServiceMessage>);
  void SortChats();
  void Merge(Conversation&, const std::vector<ChatMessage>&);
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
  void Mouse();
  void EditKey(wint_t);
  void Insert(const std::wstring&);
  void Paste(const std::wstring&);
  void ChatPicker(bool forward = false);
  void RebuildPicker(bool preserveSelection = true);
  void Commands();
  // Optional Desk entries named by HARBOR_DESK_COMMANDS, each run as an argv child on this same terminal.
  std::vector<Option> DeskOptions();
  void DeskHandoff(const std::vector<std::string>& argv);
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
};
