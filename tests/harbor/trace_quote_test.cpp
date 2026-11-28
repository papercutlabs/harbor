// A picker result's title reaches the Duet app as JSON written by HarborTrace::Quote.
#include "trace.h"
#include "cereal/external/rapidjson/document.h"
#include <cassert>
int main()
{
  // Doug's chat name carries an em dash; each of its three UTF-8 bytes was once escaped as its own \u00XX.
  const std::string title = "Doug (Chief of Staff) \xe2\x80\x94 test";
  const std::string result = "{\"schema\":\"pcl.duet.picker-result.v1\",\"action\":" + HarborTrace::Quote("assign") +
    ",\"title\":" + HarborTrace::Quote(title) + ",\"slot\":1}";
  CEREAL_RAPIDJSON_NAMESPACE::Document decoded;
  decoded.Parse(result.c_str());
  assert(!decoded.HasParseError() && decoded["title"].IsString());
  assert(std::string(decoded["title"].GetString(), decoded["title"].GetStringLength()) == title);
  return 0;
}
