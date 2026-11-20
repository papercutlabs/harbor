#pragma once
#include <string>
namespace HarborRestart
{
// Wait for the backend all updating windows just left, before ordinary connect-or-spawn.
bool WaitForBackend(const std::string& appDir);
}
