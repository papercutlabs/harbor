#pragma once
#include "richtext.h"
#include <cstdint>
#include <string>
#include <td/telegram/td_api.h>

namespace TelegramRichText
{
// Maps TDLib's formattedText to the protocol's native RichText encoding. Offsets stay in TDLib's
// UTF-16 units; entity types Harbor does not present (mentions, hashtags, custom emoji…) are
// omitted, leaving their text literal.
inline RichText::Message
Convert(const td::td_api::formattedText& p_FormattedText)
{
  RichText::Message message;
  message.text = p_FormattedText.text_;
  for (const auto& entity : p_FormattedText.entities_)
  {
    if (!entity || !entity->type_)
      continue;
    RichText::Entity out;
    out.offset = entity->offset_;
    out.length = entity->length_;
    switch (entity->type_->get_id())
    {
      case td::td_api::textEntityTypeBold::ID: out.kind = RichText::Bold; break;
      case td::td_api::textEntityTypeItalic::ID: out.kind = RichText::Italic; break;
      case td::td_api::textEntityTypeUnderline::ID: out.kind = RichText::Underline; break;
      case td::td_api::textEntityTypeStrikethrough::ID: out.kind = RichText::Strike; break;
      case td::td_api::textEntityTypeSpoiler::ID: out.kind = RichText::Spoiler; break;
      case td::td_api::textEntityTypeCode::ID: out.kind = RichText::Code; break;
      case td::td_api::textEntityTypePre::ID: out.kind = RichText::Pre; break;
      case td::td_api::textEntityTypePreCode::ID:
        out.kind = RichText::Pre;
        out.arg = static_cast<const td::td_api::textEntityTypePreCode&>(*entity->type_).language_;
        break;
      case td::td_api::textEntityTypeBlockQuote::ID: out.kind = RichText::Quote; break;
      case td::td_api::textEntityTypeExpandableBlockQuote::ID: out.kind = RichText::ExpandableQuote; break;
      case td::td_api::textEntityTypeTextUrl::ID:
        out.kind = RichText::TextUrl;
        out.arg = static_cast<const td::td_api::textEntityTypeTextUrl&>(*entity->type_).url_;
        break;
      case td::td_api::textEntityTypeUrl::ID: out.kind = RichText::Url; break;
      case td::td_api::textEntityTypeEmailAddress::ID: out.kind = RichText::Email; break;
      default: continue;
    }
    message.entities.push_back(out);
  }
  return message;
}

inline std::string
Encode(const td::td_api::formattedText& p_FormattedText)
{
  return RichText::Encode(Convert(p_FormattedText));
}

inline size_t
Utf16PosToBytePos(const std::string& p_Str, int32_t p_Utf16Pos)
{
  int32_t utf16Units = 0;
  size_t i = 0;
  while (i < p_Str.size() && utf16Units < p_Utf16Pos)
  {
    unsigned char c = static_cast<unsigned char>(p_Str[i]);
    if (c < 0x80)
    {
      i += 1;
      utf16Units += 1;
    }
    else if (c < 0xE0)
    {
      i += 2;
      utf16Units += 1;
    }
    else if (c < 0xF0)
    {
      i += 3;
      utf16Units += 1;
    }
    else
    {
      i += 4;
      utf16Units += 2; // surrogate pair = 2 UTF-16 code units
    }
  }
  return i;
}

// Adjusts an entity's UTF-16 range for text inserted around the mention [p_Offset, p_Offset + p_Length):
// p_Prefix units at the mention start and p_Suffix units at the mention end.
inline void
ShiftEntity(td::td_api::textEntity& p_Other, int32_t p_Offset, int32_t p_Length, int32_t p_Prefix, int32_t p_Suffix)
{
  const int32_t mentionEnd = p_Offset + p_Length;
  const int32_t otherEnd = p_Other.offset_ + p_Other.length_;
  if (otherEnd <= p_Offset)
  {
    // Entity entirely before mention - no change
  }
  else if (p_Other.offset_ >= mentionEnd)
  {
    // Entity entirely after mention - shift offset
    p_Other.offset_ += p_Prefix + p_Suffix;
  }
  else if (p_Other.offset_ <= p_Offset)
  {
    // Entity starts before (or at) the mention and ends inside or after it - it covers the prefix,
    // and the suffix only when it reaches the mention end
    p_Other.length_ += p_Prefix + ((otherEnd >= mentionEnd) ? p_Suffix : 0);
  }
  else
  {
    // Entity starts inside mention - shift by prefix, extend by suffix only when it reaches the end
    p_Other.offset_ += p_Prefix;
    if (otherEnd >= mentionEnd)
    {
      p_Other.length_ += p_Suffix;
    }
  }
}

// Expands textEntityTypeMentionName entities into literal "@name" (or "@[first last]" when
// p_BracketQuote and the name has spaces) text, shifting the other entities, and drops mention
// entities so getMarkdownText and Convert see plain text there.
inline void
ExpandMentions(td::td_api::formattedText& p_FormattedText, bool p_BracketQuote)
{
  auto& text = p_FormattedText.text_;
  auto& entities = p_FormattedText.entities_;

  for (int i = static_cast<int>(entities.size()) - 1; i >= 0; --i)
  {
    auto& entity = entities[i];
    if (!entity || !entity->type_)
      continue;
    const auto entityId = entity->type_->get_id();

    if (entityId == td::td_api::textEntityTypeMentionName::ID)
    {
      const int32_t utf16Offset = entity->offset_;
      const int32_t utf16Length = entity->length_;

      // Convert UTF-16 offset to byte position
      size_t byteOffset = Utf16PosToBytePos(text, utf16Offset);
      size_t byteEnd = Utf16PosToBytePos(text, utf16Offset + utf16Length);
      std::string mentionText = text.substr(byteOffset, byteEnd - byteOffset);

      bool hasSpaces = (mentionText.find(' ') != std::string::npos);
      std::string prefix;
      std::string suffix;
      if (p_BracketQuote && hasSpaces)
      {
        prefix = "@[";
        suffix = "]";
      }
      else
      {
        prefix = "@";
      }

      // Insert suffix first (at higher position), then prefix
      text.insert(byteEnd, suffix);
      text.insert(byteOffset, prefix);

      const int32_t prefixUtf16Len = static_cast<int32_t>(prefix.size());
      const int32_t suffixUtf16Len = static_cast<int32_t>(suffix.size());

      // Adjust offsets of other entities
      for (int j = 0; j < static_cast<int>(entities.size()); ++j)
      {
        if (j == i || !entities[j]) continue;
        ShiftEntity(*entities[j], utf16Offset, utf16Length, prefixUtf16Len, suffixUtf16Len);
      }

      // Erase the mention entity
      entities.erase(entities.begin() + i);
    }
    else if (entityId == td::td_api::textEntityTypeMention::ID)
    {
      // Text already has @username, just remove the entity so getMarkdownText works
      entities.erase(entities.begin() + i);
    }
  }
}
}
