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

// launchInActiveConsoleSessionElevated:把进程拉到活动控制台会话的**交互桌面**,
// 并尽量免 UAC —— 用 GetTokenInformation(TokenLinkedToken) 取活动会话用户的
// 提权链接令牌(管理员的完整令牌)。取不到链接令牌(用户非管理员/UAC 关)时回退
// 受限令牌(可能弹 UAC,安全桌面关时 VNC 仍可点)。窗口可见(不加 CREATE_NO_WINDOW),
// 供 operator 经 VNC 人工交互(Slice E3 活动会话交互安装)。
//   used_elevated_token:出参,成功时回填是否用了提权链接令牌(日志/诊断用)。
bool launchInActiveConsoleSessionElevated(const std::wstring& command_line,
                                          PROCESS_INFORMATION& process_info,
                                          bool& used_elevated_token,
                                          std::string& err);
bool launchApplicationInActiveConsoleSessionElevated(const std::wstring& application_path,
                                                     const std::wstring& arguments,
                                                     const std::wstring& working_directory,
                                                     PROCESS_INFORMATION& process_info,
                                                     bool& used_elevated_token,
                                                     std::string& diagnostics,
                                                     std::string& err);
std::wstring quoteArg(const std::string& arg);
std::wstring widen(const std::string& value);

}  // namespace device_agent::remotedesktop::windows

#endif  // _WIN32
