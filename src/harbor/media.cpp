#include "media.h"
#include "clipboard.h"
#include "fileutil.h"
#include "strutil.h"
#include "timeutil.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;

namespace
{
std::vector<char*>
Args(const std::vector<std::string>& args)
{
  std::vector<char*> result;
  for (const auto& arg : args)
    result.push_back(const_cast<char*>(arg.c_str()));
  result.push_back(nullptr);
  return result;
}
bool
Regular(const std::string& path)
{
  struct stat st;
  return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}
std::string
FFmpeg()
{
  const auto bundled = FileUtil::DirName(FileUtil::GetSelfPath()) + "/ffmpeg";
  return Regular(bundled) ? bundled : "ffmpeg";
}
int
Color(int r, int g, int b)
{
  int ri = std::max(0, std::min(5, int(std::round(r / 255.0 * 5)))),
      gi = std::max(0, std::min(5, int(std::round(g / 255.0 * 5)))),
      bi = std::max(0, std::min(5, int(std::round(b / 255.0 * 5))));
  return std::max(32, 16 + 36 * ri + 6 * gi + bi);
}
}
HarborProcess
HarborCapture(const std::vector<std::string>& args, size_t limit, int timeoutMs)
{
  HarborProcess result;
  if (args.empty())
    return result;
  int pipefd[2];
  if (pipe(pipefd))
    return result;
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
  posix_spawn_file_actions_addclose(&actions, pipefd[0]);
  posix_spawn_file_actions_addclose(&actions, pipefd[1]);
  posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
  posix_spawnattr_t attr;
  posix_spawnattr_init(&attr);
  posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
  posix_spawnattr_setpgroup(&attr, 0);
  auto argv = Args(args);
  pid_t pid = -1;
  int error = posix_spawnp(&pid, args[0].c_str(), &actions, &attr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  posix_spawnattr_destroy(&attr);
  close(pipefd[1]);
  if (error)
  {
    close(pipefd[0]);
    return result;
  }
  fcntl(pipefd[0], F_SETFL, fcntl(pipefd[0], F_GETFL) | O_NONBLOCK);
  int status = 0;
  bool done = false, eof = false, aborted = false;
  const auto start = TimeUtil::GetCurrentTimeMSec();
  std::array<char, 16384> buffer;
  while (!done || !eof)
  {
    pollfd pfd{ pipefd[0], POLLIN, 0 };
    poll(&pfd, 1, 20);
    ssize_t count;
    while ((count = read(pipefd[0], buffer.data(), buffer.size())) > 0)
    {
      if (result.output.size() + size_t(count) > limit)
      {
        aborted = true;
        break;
      }
      result.output.append(buffer.data(), count);
    }
    if (count == 0)
      eof = true;
    if (!done)
    {
      pid_t check = waitpid(pid, &status, WNOHANG);
      done = check == pid || check < 0;
    }
    if (aborted || TimeUtil::GetCurrentTimeMSec() - start > timeoutMs)
    {
      kill(-pid, SIGKILL);
      if (!done)
        waitpid(pid, &status, 0);
      aborted = true;
      break;
    }
  }
  close(pipefd[0]);
  result.status = !aborted && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  return result;
}
pid_t
HarborSpawn(const std::vector<std::string>& args)
{
  if (args.empty())
    return -1;
  auto argv = Args(args);
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  for (int fd : { STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO })
    posix_spawn_file_actions_addopen(&actions, fd, "/dev/null", fd == STDIN_FILENO ? O_RDONLY : O_WRONLY, 0);
  pid_t pid = -1;
  int error = posix_spawnp(&pid, args[0].c_str(), &actions, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  return error ? -1 : pid;
}
std::vector<std::string>
HarborPaths(const std::wstring& raw)
{
  std::string text = StrUtil::ToString(raw);
  auto expand = [](std::string p)
  {
    if (p == "~" || p.find("~/") == 0)
    {
      const char* home = getenv("HOME");
      p = std::string(home ? home : "") + p.substr(1);
    }
    if (p.find("file://") == 0)
    {
      p = p.substr(7);
      if (p.find("localhost/") == 0)
        p = p.substr(9);
      std::string decoded;
      for (size_t i = 0; i < p.size(); i++)
      {
        if (p[i] == '%' && i + 2 < p.size() && isxdigit(p[i + 1]) && isxdigit(p[i + 2]))
        {
          decoded += char(strtol(p.substr(i + 1, 2).c_str(), nullptr, 16));
          i += 2;
        }
        else
          decoded += p[i];
      }
      p = decoded;
    }
    return p;
  };
  if (text.find('/') == std::string::npos && text.find('~') != 0)
    return {};
  size_t begin = text.find_first_not_of(" \r\n\t"), end = text.find_last_not_of(" \r\n\t");
  if (begin == std::string::npos)
    return {};
  text = text.substr(begin, end - begin + 1);
  auto whole = expand(text);
  if (Regular(whole))
    return { whole };
  std::vector<std::string> paths;
  std::string token;
  char quote = 0;
  bool escape = false;
  for (char c : text)
  {
    if (escape)
    {
      token += c;
      escape = false;
      continue;
    }
    if (c == '\\' && quote != '\'')
    {
      escape = true;
      continue;
    }
    if (quote)
    {
      if (c == quote)
        quote = 0;
      else
        token += c;
      continue;
    }
    if (c == '\'' || c == '"')
    {
      quote = c;
      continue;
    }
    if (isspace(static_cast<unsigned char>(c)))
    {
      if (!token.empty())
      {
        paths.push_back(expand(token));
        token.clear();
      }
    }
    else
      token += c;
  }
  if (quote || escape)
    return {};
  if (!token.empty())
    paths.push_back(expand(token));
  if (paths.empty())
    return {};
  for (auto& p : paths)
    if (!Regular(p))
      return {};
  return paths;
}
HarborMedia::HarborMedia()
{
  const char* program = getenv("TERM_PROGRAM");
  const char* term = getenv("TERM");
  const char* overrideMode = getenv("HARBOR_GRAPHICS");
  m_Graphics = !getenv("TMUX") && ((program && std::string(program) == "ghostty") ||
                                   (term && std::string(term).find("kitty") != std::string::npos));
  if (overrideMode)
    m_Graphics = std::string(overrideMode) == "kitty";
  m_NextImageId = 0x70000000u + (static_cast<uint32_t>(getpid()) & 0x000fffffu) * 256u;
}
HarborMedia::~HarborMedia()
{
  Stop();
  ClearGraphics();
  for (auto [child, viewer] : m_Children)
  {
    int status;
    // Quick Look stays open until the user closes it.
    waitpid(child, &status, viewer == Viewer::QuickLook ? WNOHANG : 0);
  }
}
std::string
HarborMedia::CacheKey(const std::string& p, int w, int h) const
{
  return p + "\n" + std::to_string(w) + "x" + std::to_string(h);
}
void
HarborMedia::RequestImage(const std::string& path, int cols, int rows, bool video)
{
  const auto key = CacheKey(path, cols, rows);
  if (m_Images.count(key) || m_Worker.valid() || cols < 1 || rows < 1)
    return;
  // Match the PNG canvas to the terminal rectangle before placement. Some
  // terminal implementations stretch a bare image when both c and r are set.
  double cellAspect = 2.0;
  struct winsize terminal = {};
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &terminal) == 0 && terminal.ws_xpixel && terminal.ws_ypixel &&
      terminal.ws_col && terminal.ws_row)
    cellAspect =
      double(terminal.ws_ypixel) * terminal.ws_col / (double(terminal.ws_xpixel) * terminal.ws_row);
  cellAspect = std::max(1.0, std::min(4.0, cellAspect));
  int pngWidth = std::min(960, cols * 16);
  int pngHeight = std::max(2, int(std::round(pngWidth * rows * cellAspect / cols)));
  if (pngHeight > 960)
  {
    pngWidth = std::max(2, pngWidth * 960 / pngHeight);
    pngHeight = 960;
  }
  m_Worker =
    std::async(std::launch::async,
               [=]
               {
                 HarborImage image;
                 image.width = cols;
                 image.height = rows * 2;
                 image.pngWidth = pngWidth;
                 image.pngHeight = pngHeight;
                 std::vector<std::string> args = { FFmpeg(), "-v", "error", "-nostdin" };
                 if (video)
                 {
                   args.push_back("-ss");
                   args.push_back("0");
                 }
                 args.insert(args.end(),
                             { "-i",
                               path,
                               "-frames:v",
                               "1",
                               "-vf",
                               "scale=" + std::to_string(cols) + ":" + std::to_string(rows * 2) +
                                 ":force_original_aspect_ratio=decrease,pad=" + std::to_string(cols) + ":" +
                                 std::to_string(rows * 2) + ":(ow-iw)/2:(oh-ih)/2:color=0x1d2b3c",
                               "-threads",
                               "1",
                               "-f",
                               "rawvideo",
                               "-pix_fmt",
                               "rgb24",
                               "pipe:1" });
                 auto result = HarborCapture(args);
                 image.failed = result.status != 0 || result.output.size() != size_t(cols * rows * 2 * 3);
                 if (!image.failed)
                 {
                   image.pixels.assign(result.output.begin(), result.output.end());
                   if (m_Graphics)
                   {
                     auto png = HarborCapture(
                       { FFmpeg(),
                         "-v",
                         "error",
                         "-nostdin",
                         "-i",
                         path,
                         "-frames:v",
                         "1",
                         "-vf",
                         "scale=" + std::to_string(pngWidth) + ":" + std::to_string(pngHeight) +
                           ":force_original_aspect_ratio=decrease,pad=" + std::to_string(pngWidth) + ":" +
                           std::to_string(pngHeight) + ":(ow-iw)/2:(oh-ih)/2:color=0x1d2b3c,setsar=1",
                         "-threads",
                         "1",
                         "-f",
                         "image2pipe",
                         "-vcodec",
                         "png",
                         "pipe:1" },
                       4 * 1024 * 1024);
                     if (png.status == 0)
                       image.png = std::move(png.output);
                   }
                 }
                 return std::make_pair(key, image);
               });
}
const HarborImage*
HarborMedia::Image(const std::string& path, int w, int h) const
{
  auto it = m_Images.find(CacheKey(path, w, h));
  return it == m_Images.end() ? nullptr : &it->second;
}
bool
HarborMedia::Poll()
{
  bool changed = false;
  if (m_Worker.valid() && m_Worker.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready)
  {
    auto value = m_Worker.get();
    value.second.graphicsId = m_NextImageId++;
    m_Images[value.first] = std::move(value.second);
    changed = true;
  }
  if (m_AudioWorker.valid() &&
      m_AudioWorker.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready)
  {
    auto path = m_AudioWorker.get();
    if (!m_PlayingPath.empty())
    {
      if (path.empty())
      {
        m_Error = "Could not decode this voice message";
        m_PlayingPath.clear();
      }
      else
      {
#ifdef __APPLE__
        m_Player = getenv("HARBOR_TEST_MUTE") ? HarborSpawn({ "/usr/bin/afplay", "-v", "0", path })
                                              : HarborSpawn({ "/usr/bin/afplay", path });
#else
        m_Player = HarborSpawn({ "ffplay", "-nodisp", "-autoexit", "-loglevel", "error", path });
#endif
        m_PlayStart = TimeUtil::GetCurrentTimeMSec();
        m_PauseTotal = 0;
        if (m_Player < 0)
        {
          m_Error = "Audio player unavailable";
          m_PlayingPath.clear();
        }
      }
    }
    changed = true;
  }
  if (m_Player > 0)
  {
    int status;
    auto result = waitpid(m_Player, &status, WNOHANG);
    if (result == m_Player)
    {
      if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        m_Error = "Audio playback failed";
      m_Player = -1;
      m_Paused = false;
      changed = true;
    }
    else if (TimeUtil::GetCurrentTimeMSec() - m_LastPosition >= 200)
    {
      m_LastPosition = TimeUtil::GetCurrentTimeMSec();
      changed = true;
    }
  }
  m_Children.erase(std::remove_if(m_Children.begin(),
                                  m_Children.end(),
                                  [](const auto& child)
                                  {
                                    int status;
                                    return waitpid(child.first, &status, WNOHANG) != 0;
                                  }),
                   m_Children.end());
  return changed;
}
void
HarborMedia::DrawImage(const HarborImage& image, int y, int x, int cols, int rows, int sourceRow)
{
  sourceRow = std::max(0, sourceRow);
  rows = std::max(0, std::min(rows, image.height / 2 - sourceRow));
  if (m_Graphics && !image.png.empty())
    m_Placements.emplace_back(&image, y, x, cols, rows, sourceRow);
  if (image.failed || image.pixels.empty())
  {
    attrset(COLOR_PAIR(2));
    mvaddnstr(y, x, "Preview unavailable · open externally", cols);
    return;
  }
  for (int row = 0; row < std::min(rows, image.height / 2); row++)
    for (int col = 0; col < std::min(cols, image.width); col++)
    {
      size_t top = ((row + sourceRow) * 2 * image.width + col) * 3, bot = top + image.width * 3;
      const auto& p = image.pixels;
      int fg = Color(p[top], p[top + 1], p[top + 2]), bg = Color(p[bot], p[bot + 1], p[bot + 2]);
      short pair = 1;
      if (COLORS >= 256)
      {
        auto key = std::make_pair(fg, bg);
        auto it = m_Pairs.find(key);
        if (it == m_Pairs.end() && m_NextPair < std::min(COLOR_PAIRS, 32767))
        {
          pair = m_NextPair++;
          init_pair(pair, fg, bg);
          m_Pairs[key] = pair;
        }
        else if (it != m_Pairs.end())
          pair = it->second;
      }
      attrset(COLOR_PAIR(pair));
      mvaddwstr(y + row, x + col, L"▀");
    }
}
bool
HarborMedia::Play(const std::string& path)
{
  if (m_AudioWorker.valid())
  {
    m_Error = "Preparing audio; try again in a moment";
    return false;
  }
  Stop();
  m_Error.clear();
  m_PlayingPath = path;
  m_AudioWorker = std::async(
    std::launch::async,
    [path]
    {
      const auto output =
        FileUtil::GetTempDir() + "/voice-" + std::to_string(getpid()) + "-" +
        std::to_string(std::hash<std::string>{}(path)) + ".wav"; // windows share temp/
      if (Regular(output))
        return output;
      auto result = HarborCapture(
        { FFmpeg(), "-v", "error", "-nostdin", "-y", "-i", path, "-vn", "-ar", "44100", "-ac", "1", output },
        1024 * 1024,
        30000);
      return result.status == 0 && Regular(output) ? output : std::string();
    });
  return true;
}
void
HarborMedia::Stop()
{
  if (m_Player > 0)
  {
    if (m_Paused)
      kill(m_Player, SIGCONT);
    kill(m_Player, SIGTERM);
    int status;
    waitpid(m_Player, &status, 0);
  }
  m_Player = -1;
  m_Paused = false;
  m_PlayingPath.clear();
}
void
HarborMedia::TogglePause()
{
  if (m_Player <= 0)
    return;
  if (m_Paused)
  {
    kill(m_Player, SIGCONT);
    m_PauseTotal += TimeUtil::GetCurrentTimeMSec() - m_PauseStart;
  }
  else
  {
    kill(m_Player, SIGSTOP);
    m_PauseStart = TimeUtil::GetCurrentTimeMSec();
  }
  m_Paused = !m_Paused;
}
double
HarborMedia::Position() const
{
  if (m_Player <= 0)
    return 0;
  return ((m_Paused ? m_PauseStart : TimeUtil::GetCurrentTimeMSec()) - m_PlayStart - m_PauseTotal) / 1000.0;
}
bool
HarborMedia::Open(const std::string& path, Viewer viewer)
{
  // Offline tests exercise the actual spawn boundary without opening a user's player.
  const char* testOpener = getenv("HARBOR_TEST_OPENER");
  pid_t pid = -1;
  if (testOpener && getenv("HARBOR_DEMO_FIXTURES"))
    pid = HarborSpawn({ testOpener, path, viewer == Viewer::Preview ? "preview" :
                                         viewer == Viewer::QuickLook ? "quick-look" : "default" });
  else
  {
#ifdef __APPLE__
    if (viewer == Viewer::Preview)
      pid = HarborSpawn({ "/usr/bin/open", "-a", "Preview", path });
    else if (viewer == Viewer::QuickLook)
      pid = HarborSpawn({ "/usr/bin/qlmanage", "-p", path });
    else
      pid = HarborSpawn({ "/usr/bin/open", path });
#else
    pid = HarborSpawn({ "xdg-open", path });
#endif
  }
  if (pid > 0)
    m_Children.emplace_back(pid, viewer);
  else
    m_Error = "Could not launch the default viewer";
  return pid > 0;
}
bool
HarborMedia::CopyText(const std::string& text)
{
#ifdef __APPLE__
  // Tests use a private named pasteboard, never the user's general clipboard.
  const char* testName = getenv("HARBOR_TEST_PASTEBOARD");
  if (testName && getenv("HARBOR_DEMO_FIXTURES"))
  {
    const std::string script =
      "ObjC.import('AppKit'); function run(a) { var p=$.NSPasteboard.pasteboardWithName(a[0]); "
      "p.clearContents; return p.setStringForType(a[1],'public.utf8-plain-text') ? 'ok' : 'failed'; }";
    auto result =
      HarborCapture({ "/usr/bin/osascript", "-l", "JavaScript", "-e", script, testName, text }, 1024, 3000);
    return result.status == 0 && result.output.find("ok") != std::string::npos;
  }
#endif
  Clipboard::SetText(text);
  return true;
}
std::string
HarborMedia::PasteboardImage()
{
  if (getenv("HARBOR_DEMO_FIXTURES") && getenv("HARBOR_TEST_CLIPBOARD_FILE"))
    return ""; // Private text clipboard fixtures never inspect the system pasteboard.
#ifdef __APPLE__
  const auto path =
    FileUtil::GetTempDir() + "/clipboard-" + std::to_string(getpid()) + "-" +
    std::to_string(TimeUtil::GetCurrentTimeMSec()) + ".png";
  const std::string script =
    "ObjC.import('AppKit'); function run(argv) { var p=argv[1] ? $.NSPasteboard.pasteboardWithName(argv[1]) "
    ": $.NSPasteboard.generalPasteboard; var "
    "d=p.dataForType('public.png'); if(!d || d.isNil()) { var t=p.dataForType('public.tiff'); if(!t || "
    "t.isNil()) return 'none'; var b=$.NSBitmapImageRep.imageRepWithData(t); "
    "d=b.representationUsingTypeProperties(4,$()); } return d.writeToFileAtomically(argv[0],true) ? 'ok' : "
    "'none'; }";
  const char* testName = getenv("HARBOR_TEST_PASTEBOARD");
  auto result = HarborCapture(
    { "/usr/bin/osascript", "-l", "JavaScript", "-e", script, path, testName ? testName : "" }, 1024, 3000);
  if (result.status == 0 && Regular(path))
    return path;
#endif
  return "";
}

namespace
{
std::string
Base64(const std::string& data)
{
  static const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((data.size() + 2) / 3 * 4);
  for (size_t i = 0; i < data.size(); i += 3)
  {
    uint32_t n = uint32_t(static_cast<unsigned char>(data[i])) << 16;
    if (i + 1 < data.size())
      n |= uint32_t(static_cast<unsigned char>(data[i + 1])) << 8;
    if (i + 2 < data.size())
      n |= static_cast<unsigned char>(data[i + 2]);
    out += alphabet[(n >> 18) & 63];
    out += alphabet[(n >> 12) & 63];
    out += i + 1 < data.size() ? alphabet[(n >> 6) & 63] : '=';
    out += i + 2 < data.size() ? alphabet[n & 63] : '=';
  }
  return out;
}
}
void
HarborMedia::BeginFrame()
{
  m_Placements.clear();
  if (!m_Graphics)
    return;
  printf("\033[?2026h");
  for (auto id : m_VisibleGraphics)
    printf("\033_Ga=d,d=i,i=%u,q=2;\033\\", id);
  m_VisibleGraphics.clear();
}
void
HarborMedia::EndFrame()
{
  if (!m_Graphics)
    return;
  printf("\0337");
  uint32_t placement = 1;
  for (const auto& p : m_Placements)
  {
    const auto& image = *std::get<0>(p);
    if (!m_Uploaded.count(image.graphicsId))
    {
      auto encoded = Base64(image.png);
      for (size_t i = 0; i < encoded.size(); i += 4096)
      {
        size_t count = std::min(size_t(4096), encoded.size() - i);
        bool more = i + count < encoded.size();
        if (i == 0)
          printf("\033_Ga=t,f=100,i=%u,q=2,m=%d;", image.graphicsId, more ? 1 : 0);
        else
          printf("\033_Gq=2,m=%d;", more ? 1 : 0);
        fwrite(encoded.data() + i, 1, count, stdout);
        printf("\033\\");
      }
      m_Uploaded.insert(image.graphicsId);
    }
    printf("\033[%d;%dH\033_Ga=p,i=%u,p=%u,c=%d,r=%d,x=0,y=%d,w=%d,h=%d,C=1,q=2;\033\\",
           std::get<1>(p) + 1,
           std::get<2>(p) + 1,
           image.graphicsId,
           placement++,
           std::get<3>(p),
           std::get<4>(p),
           image.pngHeight * std::get<5>(p) / std::max(1, image.height / 2),
           image.pngWidth,
           image.pngHeight * (std::get<5>(p) + std::get<4>(p)) / std::max(1, image.height / 2) -
             image.pngHeight * std::get<5>(p) / std::max(1, image.height / 2));
    m_VisibleGraphics.insert(image.graphicsId);
  }
  printf("\0338\033[?2026l");
  fflush(stdout);
}
void
HarborMedia::ClearGraphics()
{
  if (!m_Graphics)
    return;
  for (auto id : m_Uploaded)
    printf("\033_Ga=d,d=I,i=%u,q=2;\033\\", id);
  printf("\033[?2026l");
  fflush(stdout);
  m_Uploaded.clear();
  m_VisibleGraphics.clear();
}
