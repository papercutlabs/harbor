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
  std::cout << "PASS native formatting: nested styles, code fences, quotes, escapes, literal punctuation, "
               "links, Unicode\n";
}
