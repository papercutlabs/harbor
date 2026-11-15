#include "format.h"
#include <cassert>
#include <iostream>
using namespace HarborFormatting;
// UTF-16 length of a UTF-8 string, the unit TDLib reports entity offsets in
static int32_t
U16(const std::string& utf8)
{
  int32_t n = 0;
  for (char32_t c : RichText::Utf32(utf8))
    n += c > 0xFFFF ? 2 : 1;
  return n;
}
static RichText::Message
Native(const std::string& text, std::vector<RichText::Entity> entities)
{
  RichText::Message m;
  m.text = text;
  m.entities = entities;
  // every test message goes through the stored encoding, as cached and delivered messages do
  RichText::Message decoded;
  assert(RichText::Decode(RichText::Encode(m), decoded));
  assert(decoded.text == m.text && decoded.entities.size() == m.entities.size());
  return decoded;
}
static std::wstring
Wide(const std::string& utf8)
{
  std::wstring out;
  for (char32_t c : RichText::Utf32(utf8))
    out += wchar_t(c);
  return out;
}
static void
NativeTests()
{
  // Producer shape from Teren's screenshot: preCode(29, 40, "shell") with no newline before the end.
  const std::string prefix = "It's in your Downloads now:\n\n", command = "sh ~/Downloads/create-hive-next-repos.sh",
                    suffix = "\n\nIt should end with Done. Paste me the output.";
  auto t = HarborFormatting::Native(
    Native(prefix + command + suffix, { { RichText::Pre, U16(prefix), U16(command), "shell" } }));
  assert(t.text == Wide(prefix) + L"‹shell›\n" + Wide(command) + Wide(suffix));
  assert(t.codes.size() == 1 && t.codes[0].content == Wide(command) && t.codes[0].arg == L"shell");
  assert(t.text.substr(t.codes[0].begin, t.codes[0].end - t.codes[0].begin) == Wide(command));
  assert(t.copy == Wide(prefix + command + suffix));
  assert(t.Plain(0, t.text.size()) == t.copy);
  assert(t.styles[t.text.find(L"It should")] == Plain);
  assert(t.styles[t.text.find(L"sh ~")] & Code);

  // UTF-16 offsets after non-BMP emoji, a skin-tone modifier and a combining accent
  const std::string lead = "😀 👍🏽 é\u0301 ", bold = "bold 🙂", tail = " tail";
  t = HarborFormatting::Native(Native(lead + bold + tail, { { RichText::Bold, U16(lead), U16(bold), "" } }));
  assert(t.text == Wide(lead + bold + tail));
  for (size_t i = 0; i < t.text.size(); ++i)
  {
    const bool inside = i >= Wide(lead).size() && i < Wide(lead + bold).size();
    assert(bool(t.styles[i] & Emphasis) == inside);
  }

  // an explicit native message without ranges stays literal, whatever it looks like
  const std::string literal = "**not bold** `not code` [label](https://x.example) > not quote\n```\nnot block```";
  t = HarborFormatting::Native(Native(literal, {}));
  assert(t.text == Wide(literal) && t.copy == Wide(literal));
  for (auto style : t.styles)
    assert(style == Plain);
  assert(t.codes.empty() && t.links.empty());

  // malformed and unknown ranges are ignored without touching unrelated text
  const std::string pair = "a😀b";
  t = HarborFormatting::Native(Native(pair,
                                      { { RichText::Bold, -1, 2, "" },
                                        { RichText::Bold, 0, 0, "" },
                                        { RichText::Bold, 2, 99, "" },
                                        { 'z', 0, 1, "" },
                                        { RichText::Italic, 2, 1, "" } }));
  assert(t.text == Wide(pair));
  assert(t.styles[0] == Plain && t.styles[2] == Plain);
  assert(t.styles[1] & Italic); // offset inside the surrogate pair covers that emoji only

  // several blocks: exact contents with tabs, indentation, Markdown-like punctuation, trailing newline
  const std::string intro = "Two steps:\n", one = "cd ~/src\n\tmake **all** _x_ `y`\n", mid = "then\n",
                    two = "def f(x):\n    return x ** 2  # \\* 你好", end = "\nDone with `inline`";
  const auto two16 = U16(intro + one + mid);
  t = HarborFormatting::Native(Native(intro + one + mid + two + end,
                                      { { RichText::Pre, U16(intro), U16(one), "" },
                                        { RichText::Pre, two16, U16(two), "python" },
                                        { RichText::Code, U16(intro + one + mid + two + "\nDone with "), 6, "" } }));
  assert(t.codes.size() == 2);
  assert(t.codes[0].content == Wide(one) && t.codes[0].arg.empty());
  assert(t.codes[1].content == Wide(two) && t.codes[1].arg == L"python");
  assert(t.copy == Wide(intro + one + mid + two + end));
  assert(t.text.find(L"‹python›\n") != std::wstring::npos);
  assert(t.Plain(0, t.text.size()) == t.copy); // header and breaks are display-only
  // a message with only inline code offers the inline span
  t = HarborFormatting::Native(Native("run ls -la now", { { RichText::Code, 4, 6, "" } }));
  assert(t.codes.size() == 1 && t.codes[0].content == L"ls -la" && !t.codes[0].block);

  // named links keep their destinations while the label stays short
  const std::string links = "see here and https://example.org/a or ops@example.com";
  t = HarborFormatting::Native(Native(links,
                                      { { RichText::TextUrl, 4, 4, "https://example.com/very/long/path?q=1" },
                                        { RichText::Url, 13, 21, "" },
                                        { RichText::Email, 38, 15, "" } }));
  assert(t.text == Wide(links) && t.copy == Wide(links));
  assert(t.links.size() == 3);
  assert(t.links[0].arg == L"https://example.com/very/long/path?q=1");
  assert(t.text.substr(t.links[0].begin, t.links[0].end - t.links[0].begin) == L"here");
  assert(t.links[1].arg == L"https://example.org/a");
  assert(t.links[2].arg == L"mailto:ops@example.com");
  assert(t.styles[4] & Link);

  // spoilers stay concealed in the body, copy and previews until revealed
  const std::string secret = "pin is 4242 ok";
  auto spoiler = Native(secret, { { RichText::Spoiler, 7, 4, "" } });
  t = HarborFormatting::Native(spoiler);
  assert(t.text == L"pin is ▒▒▒▒ ok" && t.copy == t.text && t.spoilers);
  assert(t.styles[7] & Spoiler);
  assert(RichText::Concealed(spoiler) == "pin is ▒▒▒▒ ok");
  assert(RichText::Preview(RichText::Encode(spoiler), "pin is 4242 ok") == "pin is ▒▒▒▒ ok");
  assert(RichText::Preview("", "legacy") == "legacy");
  t = HarborFormatting::Native(spoiler, true);
  assert(t.text == Wide(secret) && t.copy == Wide(secret));

  // block and expandable quotes: bars are display-only; collapsed shows the first line
  const std::string before = "Earlier:", quoted = "first line\nsecond line\nthird", after = "\nreply";
  auto q = Native(before + "\n" + quoted + after,
                  { { RichText::ExpandableQuote, U16(before + "\n"), U16(quoted), "" },
                    { RichText::Underline, 0, 7, "" },
                    { RichText::Strike, U16(before + "\n" + quoted + "\n"), 5, "" } });
  t = HarborFormatting::Native(q);
  assert(t.collapsed && t.expandable);
  assert(t.text == L"Earlier:\n│ first line …\nreply");
  assert(t.copy == Wide(before + "\n" + quoted + after));
  assert(t.styles[0] & Underline && t.styles[t.text.find(L"reply")] & Strike);
  t = HarborFormatting::Native(q, false, true);
  assert(!t.collapsed);
  assert(t.text == L"Earlier:\n│ first line\n│ second line\n│ third\nreply");
  assert(t.styles[t.text.find(L"second")] & Quote);
  const auto bar = t.text.find(L"│ second");
  assert(t.Plain(bar, bar + 8) == L"second"); // selection copy skips the quote bar

  // spans end after their last shown character, so a quote bar inside one is covered; a link that
  // starts in a collapsed quote's hidden part gets no span over the " …" marker
  const std::string lines = "first\nsecond";
  auto lq = Native(lines, { { RichText::ExpandableQuote, 0, U16(lines), "" },
                            { RichText::TextUrl, 3, 6, "https://a.example" } });
  t = HarborFormatting::Native(lq, false, true);
  assert(t.links.size() == 1 && t.text.substr(t.links[0].begin, t.links[0].end - t.links[0].begin) == L"st\n│ sec");
  auto hiddenLink = Native(lines, { { RichText::ExpandableQuote, 0, U16(lines), "" },
                                    { RichText::TextUrl, 6, 6, "https://hidden.example" } });
  t = HarborFormatting::Native(hiddenLink);
  assert(t.collapsed && t.links.empty());
  t = HarborFormatting::Native(hiddenLink, false, true);
  assert(t.links.size() == 1 && t.text.substr(t.links[0].begin, t.links[0].end - t.links[0].begin) == L"second");
  // a bare Url entity without a scheme opens as a web address, never a local path
  t = HarborFormatting::Native(Native("go to example.com now", { { RichText::Url, 6, 11, "" } }));
  assert(t.links.size() == 1 && t.links[0].arg == L"http://example.com");
  assert(t.copy == L"go to example.com now");

  // decoding: overlong forms, UTF-16 surrogates and code points past U+10FFFF become one U+FFFD each
  assert(RichText::Utf32("a\xC0\xAF" "b") == U"a\uFFFDb" && RichText::Utf32("\xED\xA0\x80") == U"\uFFFD");
  assert(RichText::Utf32("\xF4\x90\x80\x80") == U"\uFFFD" && RichText::Utf32("\xE0\x80\xAF") == U"\uFFFD");
  assert(RichText::Utf32("é😀") == U"é😀");

  // encoding: arguments with newlines/spaces survive; malformed values are not native data
  RichText::Message odd;
  odd.text = "x\ny";
  odd.entities.push_back({ RichText::TextUrl, 0, 1, "https://a.example/ x\n y" });
  RichText::Message back;
  assert(RichText::Decode(RichText::Encode(odd), back) && back.entities[0].arg == odd.entities[0].arg &&
         back.text == odd.text);
  assert(!RichText::Decode("", back) && !RichText::Decode("R1 2\nb 0 1 0\n\n", back) &&
         !RichText::Decode("R1 1\nb x 1 0\n\nt", back) && !RichText::Decode("**md**", back));
  assert(RichText::Decode("R1 0\n**md**", back) && back.text == "**md**" && back.entities.empty());
  // legacy fallback still offers code and links
  auto legacy = Parse(L"```sh\necho a```\nsee [docs](https://d.example) https://e.example");
  assert(legacy.codes.size() == 1 && legacy.codes[0].content == L"echo a" && legacy.codes[0].arg == L"sh");
  assert(legacy.links.size() == 2 && legacy.links[0].arg == L"https://d.example" &&
         legacy.links[1].arg == L"https://e.example");
  std::cout << "PASS native entities: producer pre shape, UTF-16 offsets, literal zero-entity text, malformed "
               "ranges, code blocks, links, spoilers, expandable quotes, encoding\n";
}
int
main()
{
  auto a = Parse(L"*Bold* and _italic_ and `code`.");
  assert(a.text == L"Bold and italic and code.");
  assert(a.styles[0] & Emphasis);
  assert(a.styles[a.text.find(L"italic")] & Italic);
  assert(a.styles[a.text.find(L"code")] & Code);
  a = Parse(L"**Bold _nested_** __line__ ~~old~~");
  assert(a.text == L"Bold nested line old");
  assert((a.styles[5] & (Emphasis | Italic)) == (Emphasis | Italic));
  assert(a.styles[a.text.find(L"line")] & Underline);
  assert(a.styles[a.text.rfind(L"old")] & Strike);
  a = Parse(L"# Heading\n> A quote\n```sh\necho '*literal*'\n```\nDone");
  assert(a.text == L"Heading\n│ A quote\necho '*literal*'\nDone");
  assert(a.styles[0] & Emphasis);
  assert(a.styles[a.text.find(L"quote")] & Quote);
  assert(a.styles[a.text.find(L"*literal*")] & Code);
  a = Parse(L"2 * 3 * 4 and snake_case_name and \\*literal\\* and *unclosed");
  assert(a.text == L"2 * 3 * 4 and snake_case_name and *literal* and *unclosed");
  a = Parse(L"[review](https://example.com) https://example.org");
  assert(a.text == L"review (https://example.com) https://example.org");
  assert(a.styles[0] & Link);
  a = Parse(L"```sh\n*unfinished*");
  assert(a.text == L"```sh\n*unfinished*");
  a = Parse(L"*你好 café* 🙂");
  assert(a.text == L"你好 café 🙂");
  assert(a.styles.size() == a.text.size());
  // Shapes emitted by TDLib get_markdown_v3 (td/telegram/MessageEntity.cpp): a pre/preCode
  // block opens with ``` plus its language and a newline, and closes with ``` straight after
  // its content, which need not end in a newline.
  // Exact bundled TDLib getMarkdownText output for preCode(offset 29, length 40, "shell"), run
  // offline by the parent seat via Client::execute.
  a = Parse(L"It's in your Downloads now:\n\n```shell\nsh ~/Downloads/create-hive-next-repos.sh```\n\n"
            L"It should end with Done. Paste me the output.");
  assert(a.text == L"It's in your Downloads now:\n\nsh ~/Downloads/create-hive-next-repos.sh\n\n"
                   L"It should end with Done. Paste me the output.");
  assert(a.text.find(L"```") == std::wstring::npos);
  for (auto i = a.text.find(L"sh ~"); a.text[i] != L'\n'; ++i)
    assert(a.styles[i] & Code);
  for (auto i = a.text.find(L"It should"); i < a.text.size(); ++i)
    assert(a.styles[i] == Plain);
  a = Parse(L"```shell\ngit -C ~/src log --format='%h %s' -3```\nThen run it.");
  assert(a.text == L"git -C ~/src log --format='%h %s' -3\nThen run it.");
  assert(a.styles[a.text.find(L"-3")] & Code);
  assert(a.styles[a.text.find(L"\n")] == Plain);
  assert(a.styles[a.text.find(L"Then")] == Plain);
  a = Parse(L"```\nls -la```\n```python\ndef f(x):\n    return x ** 2  # \\* `tick` 你好\n```\nDone *ok*");
  assert(a.text == L"ls -la\ndef f(x):\n    return x ** 2  # \\* `tick` 你好\nDone ok");
  assert(a.styles[a.text.find(L"ls")] & Code);
  assert(a.styles[a.text.find(L"    return")] & Code);
  assert(a.styles[a.text.find(L"你好")] & Code);
  assert(a.styles[a.text.find(L"Done")] == Plain);
  assert(a.styles[a.text.find(L"ok")] & Emphasis);
  a = Parse(L"Run:\n```sh\necho hi```after");
  assert(a.text == L"Run:\necho hi\nafter");
  assert(a.styles[a.text.find(L"after")] == Plain);
  a = Parse(L"```sh\nexit 0```");
  assert(a.text == L"exit 0");
  NativeTests();
  std::cout << "PASS native formatting: nested styles, code fences, quotes, escapes, literal punctuation, "
               "links, Unicode, TDLib pre blocks\n";
}
