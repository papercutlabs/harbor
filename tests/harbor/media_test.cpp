#include "fileutil.h"
#include "media.h"
#include "strutil.h"
#include <cassert>
#include <chrono>
#include <fstream>
#include <iostream>
#include <set>
#include <sys/stat.h>
#include <thread>

static void
Wait(HarborMedia& media, int milliseconds = 100)
{
  for (int i = 0; i < milliseconds / 10; i++)
  {
    media.Poll();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}
int
main(int argc, char** argv)
{
  assert(argc == 3);
  std::string dir = argv[1], fixtures = argv[2];
  FileUtil::SetApplicationDir(dir);
  FileUtil::MkDir(dir + "/temp");
  const std::string literal = "literal $(this must not execute); 'quoted'";
  auto result = HarborCapture({ "/usr/bin/printf", "%s", literal });
  assert(result.status == 0 && result.output == literal);
  result = HarborCapture({ "/nonexistent/harbor-test" });
  assert(result.status != 0);
  const auto start = std::chrono::steady_clock::now();
  result = HarborCapture({ "/bin/sleep", "5" }, 1024, 120);
  assert(result.status == -1);
  assert(std::chrono::steady_clock::now() - start < std::chrono::seconds(1));
  std::string file = dir + "/a file's name.png";
  std::ofstream(file) << "test";
  assert(HarborPaths(StrUtil::ToWString(file)).size() == 1);
  assert(HarborPaths(StrUtil::ToWString("\"" + file + "\"")).size() == 1);
  assert(HarborPaths(L"a normal message").empty());
  assert(HarborPaths(L"/does/not/exist").empty());
  HarborMedia media;
  for (auto name : { "harbor.png", "morning.mp4", "missing.png" })
  {
    std::string path = fixtures + "/" + name;
    media.RequestImage(path, 40, 9, std::string(name) == "morning.mp4");
    for (int i = 0; i < 300 && !media.Image(path, 40, 9); i++)
      Wait(media, 10);
    const auto* image = media.Image(path, 40, 9);
    assert(image);
    if (std::string(name) == "missing.png")
      assert(image->failed);
    else
    {
      assert(!image->failed && image->pixels.size() == 40 * 18 * 3);
      std::set<unsigned char> values(image->pixels.begin(), image->pixels.end());
      assert(values.size() > 10);
    }
  }
  assert(media.Play(fixtures + "/voice.wav"));
  for (int i = 0; i < 300 && !media.Playing(); i++)
    Wait(media, 10);
  if (!media.Playing())
  {
    std::cerr << media.Error() << "\n";
    return 2;
  }
  Wait(media, 120);
  assert(media.Position() > .05);
  media.TogglePause();
  assert(media.Paused());
  const auto position = media.Position();
  Wait(media, 150);
  assert(media.Position() == position);
  media.TogglePause();
  Wait(media, 100);
  assert(media.Position() > position);
  media.Stop();
  assert(!media.Playing());
  if (getenv("HARBOR_TEST_PASTEBOARD"))
  {
    const auto image = media.PasteboardImage();
    assert(!image.empty());
    std::ifstream png(image, std::ios::binary);
    char magic[8];
    png.read(magic, 8);
    assert(static_cast<unsigned char>(magic[0]) == 137 && std::string(magic + 1, 3) == "PNG");
  }
  std::cout << "Subprocess boundaries, timeouts, file paths, image/video decoding, voice play/pause, and "
               "image paste passed\n";
}
