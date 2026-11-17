// Native formatting through the actual MessageCache: schema 12 -> 14 migration of an existing
// database, new formatted rows, an in-sequence re-fetch and reload after a cache restart.
// Requires SQLite >= 3.35.0 at test time (ALTER TABLE ... DROP COLUMN rewinds the db to schema 12).
#include "appconfig.h"
#include "fileutil.h"
#include "messagecache.h"
#include "richtext.h"
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sqlite3.h>
#include <thread>
#include <unistd.h>

namespace
{
const std::string profile = "Telegram_test", chat = "1a2b#7";

ChatMessage
Load(const std::string& id, const std::string& expectText)
{
  // AddMessages is applied by the cache thread; wait for the row to reflect it
  for (int i = 0; i < 400; ++i)
  {
    std::vector<ChatMessage> found;
    if (MessageCache::GetOneMessage(profile, chat, id, found) && found.front().text == expectText)
      return found.front();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  std::cerr << "message " << id << " did not reach the cache\n";
  abort();
}

void
Exec(sqlite3* db, const std::string& sql)
{
  char* error = nullptr;
  if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &error) != SQLITE_OK)
  {
    std::cerr << sql << ": " << (error ? error : "?") << "\n";
    abort();
  }
}

int64_t
Scalar(sqlite3* db, const std::string& sql)
{
  sqlite3_stmt* statement = nullptr;
  assert(sqlite3_prepare_v2(db, sql.c_str(), -1, &statement, nullptr) == SQLITE_OK);
  assert(sqlite3_step(statement) == SQLITE_ROW);
  int64_t value = sqlite3_column_int64(statement, 0);
  sqlite3_finalize(statement);
  return value;
}

void
Open()
{
  MessageCache::Init();
  MessageCache::AddProfile(profile, false /*checkSequence*/, 1 /*dirVersion*/, false /*setup*/, false);
}
}

int
main()
{
  // step 1 uses ALTER TABLE ... DROP COLUMN, added in SQLite 3.35.0
  if (sqlite3_libversion_number() < 3035000)
  {
    std::cerr << "cache_richtext_test needs SQLite >= 3.35.0 for DROP COLUMN, found " << sqlite3_libversion() << "\n";
    return 1;
  }
  char base[] = "/tmp/harbor-cache-test-XXXXXX";
  assert(mkdtemp(base));
  FileUtil::SetApplicationDir(base);
  AppConfig::Init();
  AppConfig::SetBool("cache_enabled", true);
  // a unique draft store beside the cache must survive any schema change untouched
  const std::string drafts = std::string(base) + "/harbor-state";
  std::ofstream(drafts) << "draft\tkeep me exactly\n";

  // 1. create the database, then rewind it to the shipped schema 12 with a legacy row
  Open();
  MessageCache::Cleanup();
  const std::string dbPath = std::string(base) + "/history/" + profile + "/db.sqlite";
  {
    sqlite3* db = nullptr;
    assert(sqlite3_open(dbPath.c_str(), &db) == SQLITE_OK);
    Exec(db, "ALTER TABLE messages DROP COLUMN richText;");
    Exec(db, "ALTER TABLE messages DROP COLUMN quotedChatId;");
    Exec(db, "ALTER TABLE messages DROP COLUMN quotedSenderName;");
    Exec(db, "ALTER TABLE messages DROP COLUMN quotedRichText;");
    Exec(db, "UPDATE version SET schema = 12;");
    Exec(db, "INSERT INTO messages (chatId, id, senderId, text, quotedId, quotedText, quotedSender, fileInfo, "
             "timeSent, isOutgoing, isRead, isEdited, isDeleted, isPinned) VALUES ('" + chat +
             "', 'old1', 's', '**legacy** markdown', '', '', '', '', 1000, 0, 1, 0, 0, 0);");
    assert(Scalar(db, "SELECT COUNT(*) FROM pragma_table_info('messages') WHERE name = 'richText';") == 0);
    sqlite3_close(db);
  }

  // 2. reopening migrates additively: the legacy row keeps its text and has no native data
  Open();
  auto legacy = Load("old1", "**legacy** markdown");
  assert(legacy.richText.empty() && legacy.quotedChatId.empty() && legacy.quotedRichText.empty());
  {
    sqlite3* db = nullptr;
    assert(sqlite3_open(dbPath.c_str(), &db) == SQLITE_OK);
    assert(Scalar(db, "SELECT schema FROM version;") == 14);
    assert(Scalar(db, "SELECT COUNT(*) FROM pragma_table_info('messages') WHERE name = 'richText';") == 1);
    assert(Scalar(db, "SELECT COUNT(*) FROM messages;") == 1);
    sqlite3_close(db);
  }

  // 3. a native formatted message round-trips exactly, including UTF-16 offsets after emoji
  RichText::Message native;
  native.text = "🙂 run:\nsh ~/x.sh\tA*b*\n> not markup";
  native.entities = { { RichText::Pre, 8, 15, "shell" }, { RichText::TextUrl, 0, 2, "https://e.example/a b" } };
  ChatMessage m;
  m.id = "n1";
  m.senderId = "s";
  m.text = "legacy text consumers still read this";
  m.richText = RichText::Encode(native);
  m.quotedId = "original";
  m.quotedChatId = "other-chat";
  m.quotedSender = "sender";
  m.quotedSenderName = "Hidden author";
  m.quotedText = "secret";
  m.quotedRichText = RichText::Encode({ "secret", { { RichText::Spoiler, 0, 6, "" } } });
  m.timeSent = 2000;
  m.isOutgoing = false;
  MessageCache::AddMessages(profile, chat, "", { m });
  auto loaded = Load("n1", m.text);
  assert(loaded.richText == m.richText);
  assert(loaded.quotedId == m.quotedId && loaded.quotedChatId == m.quotedChatId &&
         loaded.quotedSender == m.quotedSender && loaded.quotedSenderName == m.quotedSenderName &&
         loaded.quotedText == m.quotedText && loaded.quotedRichText == m.quotedRichText);
  RichText::Message decoded;
  assert(RichText::Decode(loaded.richText, decoded) && decoded.text == native.text &&
         decoded.entities.size() == 2 && decoded.entities[0].arg == "shell" &&
         decoded.entities[1].arg == "https://e.example/a b");

  // an explicit native message without ranges stays distinct from a legacy row
  ChatMessage plain = m;
  plain.id = "n2";
  plain.text = "**typed stars**";
  plain.richText = RichText::Encode({ "**typed stars**", {} });
  plain.timeSent = 2500;
  MessageCache::AddMessages(profile, chat, "", { plain });
  assert(Load("n2", plain.text).richText == "R1 0\n**typed stars**");

  // 4. an in-sequence re-fetch of the message replaces text and ranges together. This does not prove
  // live edits: those re-fetch with sequence=false, which the message cache does not store, so the
  // cached text and richText keep the pre-edit pair until an in-sequence re-fetch (pre-existing
  // behavior for text, outside this contribution).
  RichText::Message edited;
  edited.text = "now bold";
  edited.entities = { { RichText::Bold, 4, 4, "" } };
  m.text = "now **bold**";
  m.richText = RichText::Encode(edited);
  m.isEdited = true;
  MessageCache::AddMessages(profile, chat, "", { m });
  loaded = Load("n1", "now **bold**");
  assert(loaded.richText == RichText::Encode(edited) && loaded.isEdited);

  // 5. restart: everything is read back from disk, by single fetch and by history page
  MessageCache::Cleanup();
  std::vector<ChatMessage> page;
  MessageCache::SetMessageHandler([&](std::shared_ptr<ServiceMessage> message)
  {
    if (message->GetMessageType() == NewMessagesNotifyType)
      page = std::static_pointer_cast<NewMessagesNotify>(message)->chatMessages;
  });
  Open();
  assert(MessageCache::FetchMessagesFrom(profile, chat, "", 10, true /*sync*/));
  assert(page.size() == 3);
  for (const auto& row : page)
    assert(row.id == "n1"   ? row.richText == RichText::Encode(edited)
           : row.id == "n2" ? row.richText == "R1 0\n**typed stars**"
                            : row.id == "old1" && row.richText.empty());
  MessageCache::SetMessageHandler(nullptr);
  auto restarted = Load("n1", "now **bold**");
  assert(restarted.richText == RichText::Encode(edited));
  assert(restarted.quotedChatId == m.quotedChatId && restarted.quotedSenderName == m.quotedSenderName &&
         restarted.quotedRichText == m.quotedRichText);
  assert(Load("old1", "**legacy** markdown").richText.empty());
  assert(Load("n2", plain.text).richText == "R1 0\n**typed stars**");
  MessageCache::Cleanup();

  std::ifstream in(drafts);
  std::string line;
  std::getline(in, line);
  assert(line == "draft\tkeep me exactly");
  std::cout << "PASS cache rich text: schema 12->14 additive reply/native migration keeps legacy rows, native rows, "
               "zero-entity rows, in-sequence re-fetch and restart reload (single fetch and history page) through MessageCache\n";
  return 0;
}
