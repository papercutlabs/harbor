// Producer to renderer: actual td_api objects (and TDLib's own getMarkdownText) through
// TelegramRichText::ExpandMentions (GetText's ProcessMentionEntities) and Convert, the stored encoding and
// Harbor's native renderer.
#include "format.h"
#include "richtextutil.h"
#include "replyutil.h"
#include "photoutil.h"
#include <cassert>
#include <iostream>
#include <td/telegram/Client.h>
namespace api = td::td_api;

static void
Add(api::formattedText& text, int32_t offset, int32_t length, api::object_ptr<api::TextEntityType> type)
{
  text.entities_.push_back(api::make_object<api::textEntity>(offset, length, std::move(type)));
}
static std::wstring
Wide(const std::string& utf8)
{
  std::wstring out;
  for (char32_t c : RichText::Utf32(utf8))
    out += wchar_t(c);
  return out;
}
int
main()
{
  api::photo photo;
  for (const auto& id : { "small-photo", "largest-photo" })
  {
    auto size = api::make_object<api::photoSize>();
    size->photo_ = api::make_object<api::file>();
    size->photo_->remote_ = api::make_object<api::remoteFile>();
    size->photo_->remote_->id_ = id;
    size->photo_->local_ = api::make_object<api::localFile>();
    photo.sizes_.push_back(std::move(size));
  }
  auto file = TelegramPhoto::File(photo);
  assert(file.fileId == "largest-photo" && file.fileType == "image/jpeg" &&
         file.filePath == "[Photo]" && file.fileStatus == FileStatusNotDownloaded);
  photo.sizes_.back()->photo_->local_->path_ = "/cache/original.jpg";
  file = TelegramPhoto::File(photo);
  assert(file.fileId == "largest-photo" && file.fileType == "image/jpeg" &&
         file.filePath == "/cache/original.jpg" && file.fileStatus == FileStatusDownloaded);

  api::messageReplyToMessage reply;
  reply.chat_id_ = -1234;
  reply.message_id_ = 5678;
  reply.quote_ = api::make_object<api::textQuote>();
  reply.quote_->text_ = api::make_object<api::formattedText>();
  reply.quote_->text_->text_ = "hidden quote";
  Add(*reply.quote_->text_, 0, 6, api::make_object<api::textEntityTypeSpoiler>());
  reply.origin_ = api::make_object<api::messageOriginHiddenUser>("Original author");
  ChatMessage convertedReply;
  TelegramReply::Convert(reply, convertedReply);
  assert(convertedReply.quotedId == StrUtil::NumToHex(int64_t(5678)) &&
         convertedReply.quotedChatId == StrUtil::NumToHex(int64_t(-1234)) &&
         convertedReply.quotedSenderName == "Original author" && convertedReply.quotedText == "hidden quote");
  RichText::Message quote;
  assert(RichText::Decode(convertedReply.quotedRichText, quote));
  assert(HarborFormatting::Native(quote).copy.find(L"hidden") == std::wstring::npos);
  reply.chat_id_ = 0;
  reply.message_id_ = 0;
  TelegramReply::Convert(reply, convertedReply);
  assert(convertedReply.quotedChatId == "?");

  // Teren's screenshot: preCode directly followed by prose
  const std::string prefix = "It's in your Downloads now:\n\n", command = "sh ~/Downloads/create-hive-next-repos.sh",
                    suffix = "\n\nIt should end with Done. Paste me the output.";
  api::formattedText shot;
  shot.text_ = prefix + command + suffix;
  Add(shot, int32_t(prefix.size()), int32_t(command.size()), api::make_object<api::textEntityTypePreCode>("shell"));
  RichText::Message decoded;
  assert(RichText::Decode(TelegramRichText::Encode(shot), decoded));
  auto view = HarborFormatting::Native(decoded);
  assert(view.codes.size() == 1 && view.codes[0].content == Wide(command) && view.codes[0].arg == L"shell");
  assert(view.copy == Wide(shot.text_) && view.text.find(L"```") == std::wstring::npos);
  assert(view.styles[view.text.find(L"It should")] == HarborFormatting::Plain);

  // every presented entity type, after a surrogate pair (UTF-16 offsets as TDLib reports them)
  api::formattedText all;
  all.text_ = "😀 bold ital under gone secret code link quote more";
  Add(all, 3, 4, api::make_object<api::textEntityTypeBold>());
  Add(all, 8, 4, api::make_object<api::textEntityTypeItalic>());
  Add(all, 13, 5, api::make_object<api::textEntityTypeUnderline>());
  Add(all, 19, 4, api::make_object<api::textEntityTypeStrikethrough>());
  Add(all, 24, 6, api::make_object<api::textEntityTypeSpoiler>());
  Add(all, 31, 4, api::make_object<api::textEntityTypeCode>());
  Add(all, 36, 4, api::make_object<api::textEntityTypeTextUrl>("https://example.com/destination"));
  Add(all, 41, 5, api::make_object<api::textEntityTypeBlockQuote>());
  Add(all, 47, 4, api::make_object<api::textEntityTypeExpandableBlockQuote>());
  Add(all, 0, 2, api::make_object<api::textEntityTypeMentionName>(42)); // not presented: left literal
  auto message = TelegramRichText::Convert(all);
  assert(message.text == all.text_ && message.entities.size() == 9);
  assert(message.entities[6].kind == RichText::TextUrl && message.entities[6].arg == "https://example.com/destination");
  assert(message.entities[0].offset == 3 && message.entities[0].length == 4);
  view = HarborFormatting::Native(message);
  using namespace HarborFormatting;
  assert(view.styles[view.text.find(L"bold")] & Emphasis && view.styles[view.text.find(L"ital")] & Italic);
  assert(view.styles[view.text.find(L"under")] & Underline && view.styles[view.text.find(L"gone")] & Strike);
  assert(view.text.find(L"secret") == std::wstring::npos && view.spoilers);
  assert(view.links.size() == 1 && view.links[0].arg == L"https://example.com/destination");
  assert(view.styles[view.text.find(L"quote")] & Quote && view.expandable);

  // GetText's producer ordering: ProcessMentionEntities (TelegramRichText::ExpandMentions) then Encode.
  // A quoted mention "Ann Lee" becomes "@[Ann Lee]" (prefix 2, suffix 1 UTF-16 units) after an emoji.
  api::formattedText mention;
  mention.text_ = "😀 ab Ann Lee cd ef";
  Add(mention, 3, 4, api::make_object<api::textEntityTypeBold>());  // "ab A": starts before, ends inside
  Add(mention, 10, 2, api::make_object<api::textEntityTypeBold>()); // "Le": wholly inside, short of the end
  Add(mention, 14, 2, api::make_object<api::textEntityTypeBold>()); // "cd": after the mention
  Add(mention, 6, 7, api::make_object<api::textEntityTypeMentionName>(42));
  TelegramRichText::ExpandMentions(mention, true);
  assert(mention.text_ == "😀 ab @[Ann Lee] cd ef" && mention.entities_.size() == 3);
  message = TelegramRichText::Convert(mention);
  assert(message.entities[0].offset == 3 && message.entities[0].length == 6);  // "ab @[A" covers the prefix
  assert(message.entities[1].offset == 12 && message.entities[1].length == 2); // "Le", not grown by the suffix
  assert(message.entities[2].offset == 17 && message.entities[2].length == 2); // "cd"
  assert(RichText::Decode(TelegramRichText::Encode(mention), decoded));
  view = HarborFormatting::Native(decoded);
  assert(view.text == Wide(mention.text_));
  const auto bold = [&](const std::wstring& part, bool expected)
  {
    const size_t at = view.text.find(part);
    assert(at != std::wstring::npos);
    for (size_t i = at; i < at + part.size(); ++i)
      if (bool(view.styles[i] & Emphasis) != expected)
        return false;
    return true;
  };
  assert(bold(L"😀 ", false) && bold(L"ab @[A", true) && bold(L"nn ", false));
  assert(bold(L"Le", true) && bold(L"e] ", false));
  assert(bold(L"cd", true) && bold(L" ef", false));
  // an unquoted mention inserts only "@": a bold ending inside it still covers the prefix
  api::formattedText plain;
  plain.text_ = "😀 hi Bob yo";
  Add(plain, 3, 5, api::make_object<api::textEntityTypeBold>()); // "hi Bo"
  Add(plain, 6, 3, api::make_object<api::textEntityTypeMentionName>(7));
  TelegramRichText::ExpandMentions(plain, true);
  assert(plain.text_ == "😀 hi @Bob yo");
  message = TelegramRichText::Convert(plain);
  assert(message.entities.size() == 1 && message.entities[0].offset == 3 && message.entities[0].length == 6);

  // the legacy text path still serializes through TDLib's own getMarkdownText for nchat
  auto legacy = api::make_object<api::formattedText>();
  legacy->text_ = shot.text_;
  Add(*legacy, int32_t(prefix.size()), int32_t(command.size()), api::make_object<api::textEntityTypePreCode>("shell"));
  auto response = td::Client::execute({ 2, api::make_object<api::getMarkdownText>(std::move(legacy)) });
  assert(response.object && response.object->get_id() == api::formattedText::ID);
  const auto& markdown = static_cast<api::formattedText&>(*response.object).text_;
  assert(markdown.find("```shell\n" + command + "```") != std::string::npos);
  std::cout << "PASS td_api formattedText -> RichText -> Harbor renderer; legacy getMarkdownText text unchanged\n";
}
