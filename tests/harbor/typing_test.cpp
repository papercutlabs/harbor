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
  assert(!a.Expire(6999) && a.Active());
  a.Update("user", true, 6900);
  assert(!a.Expire(7000) && a.Active());
  a.Complete("user", -1); assert(a.Active());
  assert(a.Expire(12900) && !a.Active());
  a.Update("", true, 0); assert(!a.Active());
  assert(HarborTyping::Phase(0) == 0 && HarborTyping::Phase(240) == 1 &&
         HarborTyping::Phase(480) == 2 && HarborTyping::Phase(720) == 0);
  std::cout << "PASS typing renewal, expiry, stop, sender completion, isolation and animation phases\n";
}
