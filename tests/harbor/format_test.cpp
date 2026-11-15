#include "format.h"
#include <cassert>
#include <iostream>
using namespace HarborFormatting;
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
  std::cout << "PASS native formatting: nested styles, code fences, quotes, escapes, literal punctuation, "
               "links, Unicode, TDLib pre blocks\n";
}
