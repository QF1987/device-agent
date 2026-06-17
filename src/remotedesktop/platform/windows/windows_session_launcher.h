#pragma once

#ifdef _WIN32

#include <windows.h>

#include <string>

namespace device_agent::remotedesktop::windows {

bool currentProcessIsSessionZero();
std::string currentExecutablePath();
bool launchInActiveConsoleSession(const std::wstring& command_line,
                                  PROCESS_INFORMATION& process_info,
                                  std::string& err);
std::wstring quoteArg(const std::string& arg);
std::wstring widen(const std::string& value);

}  // namespace device_agent::remotedesktop::windows

#endif  // _WIN32
