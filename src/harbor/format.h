#pragma once
#include "richtext.h"
#include <cwctype>
#include <string>
#include <vector>

namespace HarborFormatting
{
enum Style : unsigned
{
  Plain = 0,
  Emphasis = 1,
  Italic = 2,
  Code = 4,
  Quote = 8,
  Link = 16,
  Strike = 32,
  Underline = 64,
  Spoiler = 128
};
// A code block or link: [begin, end) in Text::text, plus its language or destination and, for
// code, the exact literal contents to copy.
struct Span
{
  size_t begin = 0, end = 0;
  std::wstring arg, content;
  bool block = false;
};
struct Text
{
  std::wstring text;
  std::vector<unsigned> styles;
  // display-only characters (quote bars, code headers, inserted breaks) never reach the clipboard
  std::vector<bool> decor;
  std::vector<Span> codes, links;
  bool native = false, spoilers = false, collapsed = false, expandable = false;
  std::wstring copy; // whole-message copy text
  void Add(wchar_t c, unsigned style, bool decoration = false)
  {
    text += c;
    styles.push_back(style);
    decor.push_back(decoration);
  }
  void Add(const std::wstring& value, unsigned style, bool decoration = false)
  {
    for (auto c : value)
      Add(c, style, decoration);
  }
  // the copyable characters of [begin, end)
  std::wstring Plain(size_t begin, size_t end) const
  {
    std::wstring out;
    for (size_t i = begin; i < end && i < text.size(); ++i)
      if (!decor[i])
        out += text[i];
    return out;
  }
};
inline bool
Escaped(const std::wstring& s, size_t at)
{
  size_t count = 0;
  while (at && s[--at] == L'\\')
    ++count;
  return count % 2;
}
inline void
Inline(const std::wstring& s, size_t begin, size_t end, unsigned style, Text& out, int depth = 0)
{
  for (size_t i = begin; i < end;)
  {
    if (s[i] == L'\\' && i + 1 < end && iswpunct(s[i + 1]))
    {
      out.Add(s[i + 1], style);
      i += 2;
      continue;
    }
    if (depth < 8 && s[i] == L'[')
    {
      size_t label = s.find(L"](", i + 1),
             close = label == std::wstring::npos ? label : s.find(L')', label + 2);
      if (close < end && label > i + 1 && close > label + 2)
      {
        size_t from = out.text.size();
        Inline(s, i + 1, label, style | Link, out, depth + 1);
        out.Add(L" (" + s.substr(label + 2, close - label - 2) + L")", style | Link);
        out.links.push_back({ from, out.text.size(), s.substr(label + 2, close - label - 2), L"" });
        i = close + 1;
        continue;
      }
    }
    std::wstring token;
    unsigned flag = Plain;
    switch (s[i])
    {
      case L'*':
        token = (i + 1 < end && s[i + 1] == L'*') ? L"**" : L"*";
        flag = Emphasis;
        break;
      case L'_':
        token = (i + 1 < end && s[i + 1] == L'_') ? L"__" : L"_";
        flag = token.size() == 2 ? Underline : Italic;
        break;
      case L'~':
        token = (i + 1 < end && s[i + 1] == L'~') ? L"~~" : L"~";
        flag = Strike;
        break;
      case L'`':
        token = L"`";
        flag = Code;
        break;
    }
    if (!token.empty() && depth < 8 && i + token.size() < end && !iswspace(s[i + token.size()]) &&
        (flag == Code || i == begin || !iswalnum(s[i - 1])))
    {
      size_t close = s.find(token, i + token.size());
      while (close < end && (Escaped(s, close) || iswspace(s[close - 1])))
        close = s.find(token, close + token.size());
      if (close < end && close > i + token.size())
      {
        if (flag == Code)
        {
          auto literal = s.substr(i + 1, close - i - 1);
          out.codes.push_back({ out.text.size(), out.text.size() + literal.size(), L"", literal });
          out.Add(literal, style | Code);
        }
        else
          Inline(s, i + token.size(), close, style | flag, out, depth + 1);
        i = close + token.size();
        continue;
      }
    }
    if (s.compare(i, 8, L"https://") == 0 || s.compare(i, 7, L"http://") == 0)
    {
      size_t stop = i;
      while (stop < end && !iswspace(s[stop]))
        ++stop;
      out.links.push_back({ out.text.size(), out.text.size() + stop - i, s.substr(i, stop - i), L"" });
      out.Add(s.substr(i, stop - i), style | Link);
      i = stop;
      continue;
    }
    out.Add(s[i++], style);
  }
}
// offer code blocks for copying when present, otherwise inline code spans
inline void
Finish(Text& out)
{
  if (out.copy.empty() && !out.native)
    out.copy = out.Plain(0, out.text.size());
  bool blocks = false;
  for (const auto& code : out.codes)
    blocks |= code.block;
  if (blocks)
  {
    std::vector<Span> kept;
    for (const auto& code : out.codes)
      if (code.block)
        kept.push_back(code);
    out.codes = kept;
  }
}
// Display-only spacing for the history view: the positions of the line breaks in Text::text after
// which one blank row is shown. Adjacent numbered items get a gap where none exists, and a
// standalone heading (a line wholly in emphasis, as "# " headings and native bold lines are) gets
// one before following text. Lines in code or quotes never count, an existing blank line is never
// doubled and a soft wrap is never a boundary, since only logical lines are considered.
inline std::vector<size_t>
Spacers(const Text& t)
{
  struct Line
  {
    size_t begin, end;
    bool blank = true, framed = false, numbered = false, listed = false, heading = false;
  };
  std::vector<Line> lines;
  for (size_t begin = 0;;)
  {
    size_t end = t.text.find(L'\n', begin);
    if (end == std::wstring::npos)
      end = t.text.size();
    Line line{ begin, end };
    bool emphasis = true;
    size_t first = end;
    for (size_t i = begin; i < end; ++i)
    {
      // a quote bar or code header, or a line opening in code or a quote, frames the line
      if (t.decor[i] && (t.styles[i] & Quote))
        line.framed = true;
      if (iswspace(t.text[i]))
        continue;
      line.blank = false;
      if (t.decor[i])
        continue;
      if (first == end)
      {
        first = i;
        line.framed |= (t.styles[i] & (Code | Quote)) != 0;
      }
      emphasis &= (t.styles[i] & Emphasis) != 0;
    }
    if (first < end && !line.framed)
    {
      size_t digits = first;
      while (digits < end && iswdigit(t.text[digits]))
        ++digits;
      line.numbered = digits > first && digits + 1 < end && (t.text[digits] == L'.' || t.text[digits] == L')') &&
                      t.text[digits + 1] == L' ';
      line.listed = line.numbered || (first + 1 < end && t.text[first + 1] == L' ' &&
                        (t.text[first] == L'-' || t.text[first] == L'*' || t.text[first] == L'•'));
      line.heading = emphasis && !line.listed;
    }
    lines.push_back(line);
    if (end == t.text.size())
      break;
    begin = end + 1;
  }
  std::vector<size_t> out;
  bool item = false;
  for (size_t n = 0; n + 1 < lines.size(); ++n)
  {
    const auto &line = lines[n], &next = lines[n + 1];
    if (line.blank || line.framed)
      item = false;
    item |= line.numbered;
    if (next.blank)
      continue;
    // a run of emphasized lines is a bold paragraph, not a heading
    const bool heading = line.heading && !next.heading && (n == 0 || !lines[n - 1].heading);
    if ((!line.blank && !line.framed && !line.listed && next.listed) || (item && next.numbered) || heading)
      out.push_back(line.end);
  }
  return out;
}
inline Text
Parse(const std::wstring& source)
{
  Text out;
  size_t start = 0;
  while (start < source.size())
  {
    if (source.compare(start, 3, L"```") == 0)
    {
      // TDLib's getMarkdownText closes a pre block directly after its content, so the closing
      // fence need not start a line; a newline just before it is the block's own line break.
      size_t header = source.find(L'\n', start + 3),
             close = header == std::wstring::npos ? header : source.find(L"```", header + 1);
      if (close != std::wstring::npos)
      {
        size_t stop = close > header + 1 && source[close - 1] == L'\n' ? close - 1 : close;
        auto literal = source.substr(header + 1, stop - header - 1);
        out.codes.push_back(
          { out.text.size(), out.text.size() + literal.size(), source.substr(start + 3, header - start - 3), literal, true });
        out.Add(literal, Code);
        start = close + 3;
        if (start < source.size())
        {
          // prose after the block starts its own unstyled line, as Telegram shows it
          out.Add(L'\n', Plain);
          start += source[start] == L'\n';
        }
        continue;
      }
      out.Add(source.substr(start), Code);
      break;
    }
    size_t end = source.find(L'\n', start);
    if (end == std::wstring::npos)
      end = source.size();
    size_t begin = start;
    unsigned style = Plain;
    if (source.compare(begin, 2, L"> ") == 0)
    {
      out.Add(L"│ ", Quote, true);
      begin += 2;
      style = Quote;
    }
    size_t hashes = begin;
    while (hashes < end && source[hashes] == L'#')
      ++hashes;
    if (hashes > begin && hashes - begin <= 6 && hashes < end && source[hashes] == L' ')
    {
      begin = hashes + 1;
      style |= Emphasis;
    }
    Inline(source, begin, end, style, out);
    if (end < source.size())
      out.Add(L'\n', Plain);
    start = end + 1;
  }
  Finish(out);
  return out;
}

// Shell blocks are commands; keep their language metadata for tools without a display label.
inline bool
ShellLanguage(std::wstring language)
{
  for (auto& c : language)
    c = std::towlower(c);
  return language == L"shell" || language == L"sh" || language == L"bash" || language == L"zsh";
}
// Renders a producer's original text from its native ranges (see richtext.h). Nothing is guessed
// from markup: a native message without ranges stays literal. Spoilers stay concealed unless
// revealed; a collapsed expandable quote shows its first line only.
inline Text
Native(const RichText::Message& message, bool revealed = false, bool expanded = false)
{
  Text out;
  out.native = true;
  std::wstring source;
  for (char32_t c : RichText::Utf32(message.text))
    source += wchar_t(c);
  const auto index = RichText::Utf16Index(source);
  std::vector<unsigned> style(source.size(), Plain);
  std::vector<int> pre(source.size(), -1), quote(source.size(), -1), link(source.size(), -1);
  std::vector<std::wstring> languages, destinations;
  std::vector<bool> expandableQuotes;
  for (const auto& entity : message.entities)
  {
    size_t begin = 0, end = 0;
    if (!RichText::KnownKind(entity.kind) || !RichText::Range(source, index, entity, begin, end))
      continue;
    unsigned flag = Plain;
    switch (entity.kind)
    {
      case RichText::Bold: flag = Emphasis; break;
      case RichText::Italic: flag = Italic; break;
      case RichText::Underline: flag = Underline; break;
      case RichText::Strike: flag = Strike; break;
      case RichText::Spoiler: flag = Spoiler; out.spoilers = true; break;
      case RichText::Code: flag = Code; break;
      case RichText::Pre:
        flag = Code;
        languages.push_back(std::wstring());
        for (char32_t c : RichText::Utf32(entity.arg))
          languages.back() += wchar_t(c);
        for (size_t i = begin; i < end; ++i)
          pre[i] = int(languages.size() - 1);
        break;
      case RichText::Quote:
      case RichText::ExpandableQuote:
        flag = Quote;
        expandableQuotes.push_back(entity.kind == RichText::ExpandableQuote);
        out.expandable |= expandableQuotes.back();
        for (size_t i = begin; i < end; ++i)
          quote[i] = int(expandableQuotes.size() - 1);
        break;
      default:
      {
        flag = Link;
        std::wstring destination;
        if (entity.kind == RichText::TextUrl)
          for (char32_t c : RichText::Utf32(entity.arg))
            destination += wchar_t(c);
        else
          destination = (entity.kind == RichText::Email ? L"mailto:" : L"") + source.substr(begin, end - begin);
        // a bare Url entity such as example.com is a web address, never a local path for the opener
        if (entity.kind == RichText::Url && destination.find(L"://") == std::wstring::npos)
          destination = L"http://" + destination;
        destinations.push_back(destination);
        for (size_t i = begin; i < end; ++i)
          link[i] = int(destinations.size() - 1);
        break;
      }
    }
    for (size_t i = begin; i < end; ++i)
      style[i] |= flag;
  }
  // inline code spans, recorded separately from pre blocks
  std::vector<std::pair<size_t, size_t>> inlineCodes;
  for (size_t i = 0; i < source.size();)
  {
    if ((style[i] & Code) && pre[i] < 0)
    {
      size_t j = i;
      while (j < source.size() && (style[j] & Code) && pre[j] < 0)
        ++j;
      inlineCodes.push_back({ i, j });
      i = j;
    }
    else
      ++i;
  }
  out.copy.clear();
  for (size_t i = 0; i < source.size(); ++i)
    out.copy += (style[i] & Spoiler) && !revealed && source[i] != L'\n' ? L'▒' : source[i];
  bool hidden = false;
  int openPre = -1, openCode = -1, openLink = -1, linkId = -1;
  size_t codeUntil = 0;
  for (size_t i = 0; i < source.size(); ++i)
  {
    const bool lineStart = i == 0 || source[i - 1] == L'\n';
    // a block starts on its own line and prose after it starts another one
    if (pre[i] >= 0 && (i == 0 || pre[i - 1] != pre[i]))
    {
      if (!out.text.empty() && out.text.back() != L'\n')
        out.Add(L'\n', Plain, true);
      Span span;
      span.block = true;
      span.arg = languages[pre[i]];
      size_t j = i;
      while (j < source.size() && pre[j] == pre[i])
        ++j;
      for (size_t k = i; k < j; ++k)
        span.content += (style[k] & Spoiler) && !revealed && source[k] != L'\n' ? L'▒' : source[k];
      if (!span.arg.empty() && !ShellLanguage(span.arg))
        out.Add(L"‹" + span.arg + L"›\n", Quote, true);
      span.begin = span.end = out.text.size();
      out.codes.push_back(span);
      openPre = int(out.codes.size() - 1);
    }
    if (i > 0 && pre[i - 1] >= 0 && pre[i] != pre[i - 1] && source[i] != L'\n' && out.text.back() != L'\n')
      out.Add(L'\n', Plain, true);
    if (quote[i] >= 0)
    {
      const bool first = i == 0 || quote[i - 1] != quote[i];
      if (first)
        hidden = false;
      if (hidden)
      {
        if (i + 1 == source.size() || quote[i + 1] != quote[i])
          out.Add(L" …", Quote, true);
        continue;
      }
      if (first && !out.text.empty() && out.text.back() != L'\n')
        out.Add(L'\n', Plain, true);
      if (first || lineStart)
        out.Add(L"│ ", Quote, true);
      bool more = false;
      for (size_t k = i + 1; k < source.size() && quote[k] == quote[i] && !more; ++k)
        more = source[k] != L'\n';
      if (expandableQuotes[quote[i]] && !expanded && !first && source[i] == L'\n' && more)
      {
        hidden = true;
        out.collapsed = true;
        continue;
      }
    }
    else if (i > 0 && quote[i - 1] >= 0 && source[i] != L'\n' && out.text.back() != L'\n')
      out.Add(L'\n', Plain, true);
    // spans start at their first shown character and end after their last one, so quote bars and
    // inserted breaks inside them are covered and a run starting in a collapsed quote's hidden part
    // gets no span
    for (const auto& code : inlineCodes)
      if (code.first == i)
      {
        out.codes.push_back({ out.text.size(), out.text.size(), L"",
                              out.copy.substr(code.first, code.second - code.first), false });
        openCode = int(out.codes.size() - 1);
        codeUntil = code.second;
      }
    if (link[i] >= 0 && (i == 0 || link[i - 1] != link[i]))
    {
      out.links.push_back({ out.text.size(), out.text.size(), destinations[link[i]], L"" });
      openLink = int(out.links.size() - 1);
      linkId = link[i];
    }
    wchar_t shown = (style[i] & Spoiler) && !revealed && source[i] != L'\n' ? L'▒' : source[i];
    out.Add(shown, style[i]);
    if (openPre >= 0 && pre[i] >= 0)
      out.codes[openPre].end = out.text.size();
    if (openCode >= 0 && i < codeUntil)
      out.codes[openCode].end = out.text.size();
    if (openLink >= 0 && link[i] == linkId)
      out.links[openLink].end = out.text.size();
  }
  // offered code: blocks when present, otherwise inline spans
  Finish(out);
  return out;
}
}
