#include "deskupdate.h"
#include <cassert>
#include <fstream>
#include <iostream>
#include <utime.h>

int main(int argc, char** argv)
{
  assert(argc == 2);
  using namespace HarborDeskUpdate;
  static_assert(IdleMS == 120000, "Production idle interval stays two minutes");
  State safe;
  safe.latest = safe.draftsEmpty = safe.stable = true;
  assert(!Ready(120999, 1000, safe));
  assert(Ready(121000, 1000, safe));
  assert(!Ready(1000, 1001, safe));
  assert(!Ready(121000, -1, safe));
  for (auto field : { &State::menu, &State::child, &State::routing, &State::attachments,
                     &State::mouse, &State::browse, &State::selection, &State::auth })
  {
    auto busy = safe;
    busy.*field = true;
    assert(!Ready(999999, 1000, busy));
  }
  for (auto field : { &State::latest, &State::draftsEmpty, &State::stable })
  {
    auto busy = safe;
    busy.*field = false;
    assert(!Ready(999999, 1000, busy));
  }
  const std::string path = std::string(argv[1]) + "/request";
  const pid_t owner = getppid();
  const std::string revision(40, 'a');
  const auto wallNow = [] {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
  };
  const auto write = [&](const std::string& data) {
    std::ofstream file(path); file << data; file.close(); assert(file);
    assert(chmod(path.c_str(), 0600) == 0);
  };
  const std::string valid = "pcl.desk.update.v1\t" + std::to_string(owner) + '\t' + revision + '\n';
  assert(Request(path, owner, wallNow()).empty());
  for (const auto& bad : { std::string("wrong\n"), std::string(300, 'x'), valid + "extra\n",
       valid.substr(0, valid.size() - 1), "pcl.desk.update.v1\t" + std::to_string(owner + 1) + '\t' + revision + '\n',
       "pcl.desk.update.v1\t" + std::to_string(owner) + '\t' + std::string(40, 'A') + '\n' })
  {
    write(bad); assert(Request(path, owner, wallNow()).empty());
  }
  write(valid);
  assert(Request(path, owner, wallNow()) == revision);
  chmod(path.c_str(), 0644); assert(Request(path, owner, wallNow()).empty());
  chmod(path.c_str(), 0600);
  utimbuf future{ time(nullptr) + 60, time(nullptr) + 60 };
  assert(utime(path.c_str(), &future) == 0);
  assert(Request(path, owner, wallNow()).empty());
  write(valid);
  assert(!Acknowledge(path, owner + 1, revision, wallNow()));
  assert(!Acknowledge(path, owner, std::string(40, 'b'), wallNow()));
  assert(Acknowledge(path, owner, revision, wallNow()));
  std::ifstream input(path);
  const std::string ack((std::istreambuf_iterator<char>(input)), {});
  assert(ack == "pcl.desk.updated.v1\t" + std::to_string(owner) + '\t' + revision + '\n');
  struct stat info{}; assert(stat(path.c_str(), &info) == 0 && !(info.st_mode & 077));
  unlink(path.c_str());
  assert(mkfifo(path.c_str(), 0600) == 0);
  assert(Request(path, owner, wallNow()).empty());
  unlink(path.c_str());
  std::cout << "PASS safe update clock/state gates; private bounded request, malformed/future/wrong-owner rejection and atomic exact acknowledgment\n";
}
