#ifdef _WIN32

#include "remotedesktop/platform/windows/windows_session_launcher.h"

#include <userenv.h>
#include <wtsapi32.h>

#include <sstream>
#include <vector>

namespace device_agent::remotedesktop::windows {

namespace {

std::string lastError(const char* prefix) {
    const DWORD code = GetLastError();
    std::ostringstream oss;
    oss << prefix << " failed, error=" << code;
    return oss.str();
}

struct HandleGuard {
    HANDLE handle = nullptr;
    ~HandleGuard() {
        if (handle) {
            CloseHandle(handle);
        }
    }
};

}  // namespace

bool currentProcessIsSessionZero() {
    DWORD session_id = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &session_id)) {
        return false;
    }
    return session_id == 0;
}

std::string currentExecutablePath() {
    std::vector<wchar_t> buf(MAX_PATH);
    DWORD len = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    while (len == buf.size()) {
        buf.resize(buf.size() * 2);
        len = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    }
    if (len == 0) {
        return {};
    }
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, buf.data(), static_cast<int>(len), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, buf.data(), static_cast<int>(len), out.data(), bytes, nullptr, nullptr);
    return out;
}

std::wstring widen(const std::string& value) {
    if (value.empty()) {
        return {};
    }
    const int chars = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(chars), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), chars);
    return out;
}

std::wstring quoteArg(const std::string& arg) {
    std::wstring w = widen(arg);
    std::wstring out = L"\"";
    for (wchar_t ch : w) {
        if (ch == L'"' || ch == L'\\') {
            out.push_back(L'\\');
        }
        out.push_back(ch);
    }
    out.push_back(L'"');
    return out;
}

bool launchInActiveConsoleSession(const std::wstring& command_line,
                                  PROCESS_INFORMATION& process_info,
                                  std::string& err) {
    ZeroMemory(&process_info, sizeof(process_info));
    DWORD session_id = WTSGetActiveConsoleSessionId();
    if (session_id == 0xffffffff) {
        err = "no active console session";
        return false;
    }

    HandleGuard user_token;
    if (!WTSQueryUserToken(session_id, &user_token.handle)) {
        err = lastError("WTSQueryUserToken");
        return false;
    }

    HandleGuard primary_token;
    if (!DuplicateTokenEx(user_token.handle,
                          TOKEN_ASSIGN_PRIMARY | TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ADJUST_DEFAULT |
                              TOKEN_ADJUST_SESSIONID,
                          nullptr,
                          SecurityImpersonation,
                          TokenPrimary,
                          &primary_token.handle)) {
        err = lastError("DuplicateTokenEx");
        return false;
    }

    LPVOID environment = nullptr;
    if (!CreateEnvironmentBlock(&environment, primary_token.handle, FALSE)) {
        environment = nullptr;
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.lpDesktop = const_cast<LPWSTR>(L"winsta0\\default");

    std::wstring mutable_cmd = command_line;
    // 用 CREATE_NO_WINDOW 而非 CREATE_NEW_CONSOLE:子进程是后台 RD 组件,日志已落
    // 文件(DEVICE_AGENT_RD_CHILD_LOG),不该在被控用户桌面弹一个常显的控制台窗口
    // (交互观感差)。同 windows_badge.cc 的隐藏方式。
    DWORD flags = CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW;
    BOOL ok = CreateProcessAsUserW(primary_token.handle,
                                   nullptr,
                                   mutable_cmd.data(),
                                   nullptr,
                                   nullptr,
                                   FALSE,
                                   flags,
                                   environment,
                                   nullptr,
                                   &si,
                                   &process_info);
    if (environment) {
        DestroyEnvironmentBlock(environment);
    }
    if (!ok) {
        err = lastError("CreateProcessAsUserW");
        return false;
    }
    return true;
}

}  // namespace device_agent::remotedesktop::windows

#endif  // _WIN32
