// Native failure probes exercise the production trace and shared-session functions.
// The Python owner supplies a disposable HOME/state directory and reaps this process.
#include "session.h"
#include "traceprotocol.h"
#include <cassert>
#include <csignal>
#include <stdexcept>
template<class T> static void PrivateFields(T&) { }
static void PrivateFields(SearchMessagesNotify& result)
{
  result.query = "SECRET_SEARCH_QUERY";
  result.error = "SECRET_NATIVE_ERROR";
}
template<class T> static void TypedOutcomes(uint64_t first)
{
  for (bool success : { false, true })
  {
    auto result = std::make_shared<T>("SECRET_PROFILE_NAME");
    result->requestId = first++;
    result->success = success;
    result->chatId = "SECRET_CHAT_IDENTIFIER";
    PrivateFields(*result);
    HarborTrace::Result("probe.typed_result", result, 7, 0);
  }
}
int main(int argc, char** argv)
{
  assert(argc == 3);
  HarborTrace::Start("trace-probe", "fixture", "fixture");
  const std::string mode = argv[1];
  if (mode == "spawn")
  {
    auto result = HarborSession::Attach(argv[2], "/nonexistent/harbor-native-fixture", {});
    assert(result.fd < 0);
  }
  else if (mode == "typed_outcomes")
  {
    TypedOutcomes<SearchMessagesNotify>(200);
    TypedOutcomes<FindMessageNotify>(202);
    TypedOutcomes<AvailableReactionsNotify>(204);
    TypedOutcomes<UpdatePinNotify>(206);
    TypedOutcomes<UpdateArchivedNotify>(208);
  }
  else if (mode == "exception") throw std::runtime_error("SECRET_EXCEPTION_BODY");
  else if (mode == "signal") raise(SIGSEGV);
  else
  {
    errno = EACCES;
    HarborTrace::Event("probe", {{"value", "quotation\"\ncontrol\x1b"}});
    assert(errno == EACCES);
    HarborTrace::Os("probe.os", ENOENT);
    assert(errno == EACCES);
  }
  HarborTrace::Event("process.exit", {{"status", "0"}});
}
