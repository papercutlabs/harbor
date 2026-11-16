// richtext.h
//
// Native message formatting carried beside ChatMessage::text.
//
// ChatMessage::text stays the protocol's legacy Markdown-like rendering, so nchat and other
// text consumers are unchanged. ChatMessage::richText carries the producer's original text and
// its formatting ranges, so a consumer can render them without reverse-parsing markup.
//
// Range unit: offsets and lengths are UTF-16 code units of the original text, exactly as TDLib's
// textEntity reports them. Utf16Index() is the one conversion to code-point positions.
//
// Encoding: "R1 <count>\n", then per entity "<kind> <offset> <length> <argBytes>\n<arg>\n",
// then the UTF-8 text verbatim to the end of the string.
// An empty richText means no native data (legacy cache rows, other protocols, demo text).
// "R1 0\n<text>" is an explicit native message without formatting and must render literally.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace RichText
{
enum Kind : char
{
  Bold = 'b',
  Italic = 'i',
  Underline = 'u',
  Strike = 's',
  Spoiler = 'p',
  Code = 'c',
  Pre = 'P', // arg: language, may be empty
  Quote = 'q',
  ExpandableQuote = 'Q',
  TextUrl = 'l', // arg: destination
  Url = 'U', // destination is the covered text
  Email = 'e', // destination is mailto: + covered text
};

struct Entity
{
  char kind = 0;
  int32_t offset = 0;
  int32_t length = 0;
  std::string arg;
};

struct Message
{
  std::string text;
  std::vector<Entity> entities;
};

inline bool KnownKind(char p_Kind)
{
  switch (p_Kind)
  {
    case Bold: case Italic: case Underline: case Strike: case Spoiler: case Code: case Pre:
    case Quote: case ExpandableQuote: case TextUrl: case Url: case Email:
      return true;
    default:
      return false;
  }
}

inline std::string Encode(const Message& p_Message)
{
  std::string out = "R1 " + std::to_string(p_Message.entities.size()) + "\n";
  for (const auto& entity : p_Message.entities)
  {
    out += std::string(1, entity.kind) + " " + std::to_string(entity.offset) + " " +
      std::to_string(entity.length) + " " + std::to_string(entity.arg.size()) + "\n" + entity.arg + "\n";
  }
  return out + p_Message.text;
}

// Returns false for an empty or malformed value; the caller then has no native data.
inline bool Decode(const std::string& p_Value, Message& p_Message)
{
  p_Message = Message();
  if (p_Value.compare(0, 3, "R1 ") != 0) return false;

  size_t pos = 3;
  auto number = [&](char p_End, long long& p_Out) -> bool
  {
    size_t end = p_Value.find(p_End, pos);
    if (end == std::string::npos || end == pos || end - pos > 12) return false;
    bool negative = p_Value[pos] == '-';
    long long value = 0;
    for (size_t i = pos + negative; i < end; ++i)
    {
      if (p_Value[i] < '0' || p_Value[i] > '9') return false;
      value = value * 10 + (p_Value[i] - '0');
    }
    if (negative && end == pos + 1) return false;
    p_Out = negative ? -value : value;
    pos = end + 1;
    return true;
  };

  long long count = 0;
  if (!number('\n', count) || count < 0 || count > 100000) return false;
  for (long long i = 0; i < count; ++i)
  {
    if (pos + 2 > p_Value.size() || p_Value[pos + 1] != ' ') return false;
    Entity entity;
    entity.kind = p_Value[pos];
    pos += 2;
    long long offset = 0, length = 0, argBytes = 0;
    if (!number(' ', offset) || !number(' ', length) || !number('\n', argBytes)) return false;
    if (argBytes < 0 || pos + size_t(argBytes) + 1 > p_Value.size() || p_Value[pos + argBytes] != '\n') return false;
    entity.offset = int32_t(offset);
    entity.length = int32_t(length);
    entity.arg = p_Value.substr(pos, argBytes);
    pos += argBytes + 1;
    if (offset != entity.offset || length != entity.length) return false;
    p_Message.entities.push_back(entity);
  }
  p_Message.text = p_Value.substr(pos);
  return true;
}

// For code points of a text, position k of the result is the code-point index at UTF-16
// offset k. An offset inside a surrogate pair maps to the code point containing it.
template<typename CodePoints>
inline std::vector<size_t> Utf16Index(const CodePoints& p_CodePoints)
{
  std::vector<size_t> index;
  for (size_t i = 0; i < p_CodePoints.size(); ++i)
  {
    index.push_back(i);
    if (uint32_t(p_CodePoints[i]) > 0xFFFF) index.push_back(i);
  }
  index.push_back(p_CodePoints.size());
  return index;
}

// Valid code-point range [p_Begin, p_End) for an entity, or false for malformed / empty ranges.
template<typename CodePoints>
inline bool Range(const CodePoints& p_CodePoints, const std::vector<size_t>& p_Index, const Entity& p_Entity,
                  size_t& p_Begin, size_t& p_End)
{
  if (p_Entity.offset < 0 || p_Entity.length <= 0) return false;
  const size_t begin = size_t(p_Entity.offset), end = begin + size_t(p_Entity.length);
  if (end >= p_Index.size()) return false;
  p_Begin = p_Index[begin];
  p_End = p_Index[end];
  // an end inside a surrogate pair covers that whole code point
  if (end > 0 && p_Index[end - 1] == p_End && p_End < p_CodePoints.size()) ++p_End;
  return p_End > p_Begin;
}

inline std::u32string Utf32(const std::string& p_Utf8)
{
  std::u32string out;
  for (size_t i = 0; i < p_Utf8.size();)
  {
    unsigned char c = p_Utf8[i];
    int extra = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : -1;
    char32_t cp = extra == 0 ? c : extra == 1 ? (c & 0x1F) : extra == 2 ? (c & 0x0F) : (c & 0x07);
    bool ok = extra >= 0 && i + extra < p_Utf8.size() + (extra == 0);
    for (int k = 1; ok && k <= extra; ++k)
    {
      unsigned char next = p_Utf8[i + k];
      ok = (next & 0xC0) == 0x80;
      cp = (cp << 6) | (next & 0x3F);
    }
    if (!ok)
    {
      out += U'�';
      ++i;
      continue;
    }
    // overlong encodings, UTF-16 surrogates and code points past U+10FFFF are not valid UTF-8
    static const char32_t minimum[] = { 0, 0x80, 0x800, 0x10000 };
    if (cp < minimum[extra] || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) cp = U'�';
    out += cp;
    i += extra + 1;
  }
  return out;
}

inline std::string Utf8(const std::u32string& p_Text)
{
  std::string out;
  for (char32_t cp : p_Text)
  {
    if (cp < 0x80) out += char(cp);
    else if (cp < 0x800) { out += char(0xC0 | (cp >> 6)); out += char(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { out += char(0xE0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
    else { out += char(0xF0 | (cp >> 18)); out += char(0x80 | ((cp >> 12) & 0x3F)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
  }
  return out;
}

// Plain text with every spoiler concealed, for previews and reply excerpts.
inline std::string Concealed(const Message& p_Message)
{
  std::u32string text = Utf32(p_Message.text);
  const auto index = Utf16Index(text);
  for (const auto& entity : p_Message.entities)
  {
    size_t begin = 0, end = 0;
    if (entity.kind != Spoiler || !Range(text, index, entity, begin, end)) continue;
    for (size_t i = begin; i < end; ++i)
      if (text[i] != U'\n') text[i] = U'▒';
  }
  return Utf8(text);
}

// Concealed preview of an encoded value, or p_Fallback when it carries no native data.
inline std::string Preview(const std::string& p_RichText, const std::string& p_Fallback)
{
  Message message;
  return Decode(p_RichText, message) ? Concealed(message) : p_Fallback;
}
}
