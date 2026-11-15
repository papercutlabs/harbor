#pragma once
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
  Underline = 64
};
struct Text
{
  std::wstring text;
  std::vector<unsigned> styles;
  void Add(wchar_t c, unsigned style)
  {
    text += c;
    styles.push_back(style);
  }
  void Add(const std::wstring& value, unsigned style)
  {
    for (auto c : value)
      Add(c, style);
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
        Inline(s, i + 1, label, style | Link, out, depth + 1);
        out.Add(L" (" + s.substr(label + 2, close - label - 2) + L")", style | Link);
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
          out.Add(s.substr(i + 1, close - i - 1), style | Code);
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
      out.Add(s.substr(i, stop - i), style | Link);
      i = stop;
      continue;
    }
    out.Add(s[i++], style);
  }
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
        out.Add(source.substr(header + 1, stop - header - 1), Code);
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
      out.Add(L"│ ", Quote);
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
  return out;
}
}
