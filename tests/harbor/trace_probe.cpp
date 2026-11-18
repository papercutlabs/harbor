// Native failure probes exercise the production trace and shared-session functions.
// The Python owner supplies a disposable HOME/state directory and reaps this process.
#include "session.h"
#include "trace.h"
#include <cassert>
#include <csignal>
#include <stdexcept>
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
