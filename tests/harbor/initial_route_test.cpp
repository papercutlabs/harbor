#include "launchroute.h"
#include <cassert>
#include <functional>

int main()
{
  auto rejects = [](const std::function<void()>& fn) {
    bool rejected = false;
    try { fn(); } catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);
  };
  HarborRoute::Target target{"-1001234567890", "421", "", ""};
  HarborRoute::Validate(target);
  assert(target.conversation == "ffffff16e1c4ed2e#421");
  assert(HarborRoute::SelectProfile(target, {"Signal_one", "Telegram_one"}, false) == "Telegram_one");
  rejects([&] { HarborRoute::SelectProfile(target, {"Telegram_one", "Telegram_two"}, false); });
  rejects([&] { HarborRoute::SelectProfile(target, {"Signal_one"}, false); });
  target.profile = "Telegram_two";
  assert(HarborRoute::SelectProfile(target, {"Telegram_one", "Telegram_two"}, false) == "Telegram_two");
  target.profile = "Signal_one";
  rejects([&] { HarborRoute::SelectProfile(target, {"Signal_one", "Telegram_one"}, false); });
  target.profile = "HarborDemo_local";
  rejects([&] { HarborRoute::SelectProfile(target, {"HarborDemo_local"}, false); });
  assert(HarborRoute::SelectProfile(target, {"HarborDemo_local"}, true) == "HarborDemo_local");
}
