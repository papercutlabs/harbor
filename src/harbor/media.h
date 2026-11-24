#pragma once
#include <cstdint>
#include <future>
#include <map>
#include <ncurses.h>
#include <set>
#include <string>
#include <sys/types.h>
#include <tuple>
#include <vector>

struct HarborImage
{
  int width = 0, height = 0, pngWidth = 0, pngHeight = 0;
  std::vector<unsigned char> pixels;
  bool failed = false;
  std::string png;
  uint32_t graphicsId = 0;
};
struct HarborProcess
{
  int status = -1;
  std::string output;
};
HarborProcess
HarborCapture(const std::vector<std::string>& args, size_t limit = 16 * 1024 * 1024, int timeoutMs = 15000);
pid_t
HarborSpawn(const std::vector<std::string>& args);
std::vector<std::string>
HarborPaths(const std::wstring& text);

class HarborMedia
{
public:
  HarborMedia();
  ~HarborMedia();
  void RequestImage(const std::string& path, int cols, int rows, bool video = false);
  const HarborImage* Image(const std::string& path, int cols, int rows) const;
  bool Poll();
  void BeginFrame();
  void EndFrame();
  void ClearGraphics();
  void DrawImage(const HarborImage&, int y, int x, int cols, int rows, int sourceRow = 0);
  bool Play(const std::string& path);
  void Stop();
  void TogglePause();
  bool Playing() const { return m_Player > 0; }
  bool Paused() const { return m_Paused; }
  const std::string& PlayingPath() const { return m_PlayingPath; }
  double Position() const;
  std::string Error() const { return m_Error; }
  enum class Viewer { Default, Preview, QuickLook };
  bool Open(const std::string& path, Viewer viewer = Viewer::Default);
  std::string PasteboardImage();
  bool CopyText(const std::string&);

private:
  std::string CacheKey(const std::string&, int, int) const;
  std::map<std::string, HarborImage> m_Images;
  std::future<std::pair<std::string, HarborImage>> m_Worker;
  std::future<std::string> m_AudioWorker;
  std::string m_PlayingPath, m_Error;
  pid_t m_Player = -1;
  bool m_Paused = false;
  int64_t m_LastPosition = 0;
  std::vector<std::pair<pid_t, Viewer>> m_Children;
  int64_t m_PlayStart = 0, m_PauseStart = 0, m_PauseTotal = 0;
  std::map<std::pair<int, int>, short> m_Pairs;
  short m_NextPair = 100;
  bool m_Graphics = false;
  uint32_t m_NextImageId = 0;
  std::set<uint32_t> m_Uploaded, m_VisibleGraphics;
  std::vector<std::tuple<const HarborImage*, int, int, int, int, int>> m_Placements;
};
