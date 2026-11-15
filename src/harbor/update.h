#pragma once
#include <array>
#include <cstdlib>
#include <fstream>
#include <limits.h>
#include <sstream>
#include <string>
#include <unistd.h>
namespace HarborUpdate
{
inline const char*
Version()
{
  return "0.1.12";
}
inline std::string&
RestartTarget()
{
  static std::string target;
  return target;
}
inline std::string
Resolve(const std::string& path)
{
  char resolved[PATH_MAX];
  return realpath(path.c_str(), resolved) ? resolved : "";
}
inline bool
ParseVersion(const std::string& value, std::array<int, 3>& parts)
{
  std::istringstream input(value);
  char a, b;
  return bool(input >> parts[0] >> a >> parts[1] >> b >> parts[2]) && a == '.' && b == '.' &&
         input.peek() == std::char_traits<char>::eof() && parts[0] >= 0 && parts[1] >= 0 && parts[2] >= 0;
}
inline std::string
Candidate(const std::string& current, const std::string& launcher, std::string& version)
{
  auto path = Resolve(launcher);
  const std::string suffix = "/bin/harbor";
  if (path.empty() || path == Resolve(current) || path.size() <= suffix.size() ||
      path.compare(path.size() - suffix.size(), suffix.size(), suffix) || access(path.c_str(), X_OK))
    return "";
  auto directory = path.substr(0, path.size() - suffix.size());
  auto slash = directory.find_last_of('/');
  if (slash == std::string::npos || directory.substr(0, slash).size() < 9 ||
      directory.substr(0, slash).compare(directory.substr(0, slash).size() - 9, 9, "/releases"))
    return "";
  version = directory.substr(slash + 1);
  std::ifstream metadata(directory + "/share/harbor/version");
  if (metadata)
    std::getline(metadata, version);
  std::array<int, 3> installed{}, running{};
  if (!ParseVersion(version, installed) || !ParseVersion(Version(), running) || installed <= running)
    return "";
  return path;
}
}
