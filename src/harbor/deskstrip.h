#pragma once
#include <array>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

// Per-UI opt-in. Never transported through the shared session backend.
class HarborDeskStrip
{
public:
  HarborDeskStrip()
  {
    const char* path = std::getenv("HARBOR_DESK_STRIP_FILE");
    if (path) m_Path = path;
  }
  bool Enabled() const { return !m_Path.empty(); }
  const std::array<std::wstring, 4>& Rows() const { return m_Rows; }
  bool Poll(int64_t now)
  {
    if (!Enabled() || (now >= m_LastPoll && now - m_LastPoll < 250)) return false;
    m_LastPoll = now;
    auto next = Unknown();
    Read(now, next);
    if (next == m_Rows) return false;
    m_Rows = std::move(next);
    return true;
  }

private:
  static std::array<std::wstring, 4> Unknown()
  {
    return { L"Desk  CPU ?  MEM ?  DISK ?", L"CODEX  unavailable", L"CLAUDE unavailable", L"GROK   unavailable" };
  }
  void Read(int64_t now, std::array<std::wstring, 4>& rows)
  {
    // Nonblocking open plus regular-file validation prevents a bad path from stopping input.
    const int fd = open(m_Path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return;
    struct stat info{};
    std::array<char, 16 * 1024 + 1> buffer{};
    const bool regular = fstat(fd, &info) == 0 && S_ISREG(info.st_mode) && info.st_size <= 16 * 1024;
    size_t used = 0;
    bool complete = false;
    if (regular)
      while (used < buffer.size())
      {
        const auto count = read(fd, buffer.data() + used, buffer.size() - used);
        if (count == 0) { complete = true; break; }
        if (count < 0) break;
        used += size_t(count);
      }
    close(fd);
    if (!complete || used > 16 * 1024) return;
    const std::string content(buffer.data(), used);
    const std::string prefix = "pcl.desk.strip.v1\t";
    const auto end = content.find('\n');
    if (content.compare(0, prefix.size(), prefix) || end == std::string::npos || end <= prefix.size()) return;
    int64_t stamp = 0;
    const auto parsed = std::from_chars(content.data() + prefix.size(), content.data() + end, stamp);
    if (parsed.ec != std::errc() || parsed.ptr != content.data() + end || stamp < 0 || stamp > now || now - stamp > 90000) return;
    std::array<std::wstring, 4> next;
    size_t start = end + 1;
    for (auto& row : next)
    {
      const auto newline = content.find('\n', start);
      const auto stop = newline == std::string::npos ? content.size() : newline;
      if (start >= content.size()) return;
      std::mbstate_t state{};
      while (start < stop)
      {
        wchar_t ch = 0;
        const size_t count = std::mbrtowc(&ch, content.data() + start, stop - start, &state);
        if (count == 0 || count == size_t(-1) || count == size_t(-2) ||
            ch < 0x20 || (ch >= 0x7f && ch <= 0x9f) || ch == 0x2028 || ch == 0x2029 || wcwidth(ch) < 0) return;
        row.push_back(ch);
        start += count;
      }
      start = newline == std::string::npos ? content.size() + 1 : newline + 1;
    }
    // Permit a final newline, but no fifth row (even an empty one).
    if (start < content.size()) return;
    rows = std::move(next);
  }
  std::string m_Path;
  int64_t m_LastPoll = 0;
  std::array<std::wstring, 4> m_Rows = Unknown();
};
