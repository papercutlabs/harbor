#include "typing.h"
#include <cassert>
#include <iostream>
int main()
{
  HarborTyping a, otherChat, otherProfile;
  a.Update("user", true, 0);
  assert(a.Active() && !otherChat.Active() && !otherProfile.Active());
  a.Update("second", true, 100);
  a.Complete("user", 100);
  assert(a.Active());
  a.Update("second", false, 200);
  assert(!a.Active());
  a.Update("user", true, 1000);
  a.Complete("user", -1); assert(a.Active());
  // No repeated notification is required: TDLib owns renewal and timeout.
  a.Update("other", false, 60000); assert(a.Active());
  a.Update("user", false, 61000); assert(!a.Active());
  a.Update("user", true, 62000); a.Clear(); assert(!a.Active());
  a.Update("", true, 0); assert(!a.Active());
  assert(HarborTyping::Phase(0) == 0 && HarborTyping::Phase(240) == 1 &&
         HarborTyping::Phase(480) == 2 && HarborTyping::Phase(720) == 0);
  std::cout << "PASS change-only activity, provider stop, disconnect, sender completion, isolation and animation phases\n";
}
