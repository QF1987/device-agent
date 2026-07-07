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

std::string tokenSessionDiagnostic(HANDLE token, const char* label) {
    DWORD session_id = 0;
    DWORD ret_len = 0;
    std::ostringstream oss;
    oss << label << "=";
    if (GetTokenInformation(token, TokenSessionId, &session_id, sizeof(session_id), &ret_len)) {
        oss << session_id;
    } else {
        oss << "error:" << GetLastError();
    }
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

bool launchInActiveConsoleSessionElevatedHidden(const std::wstring& command_line,
                                                PROCESS_INFORMATION& process_info,
                                                bool& used_elevated_token,
                                                std::string& err) {
    ZeroMemory(&process_info, sizeof(process_info));
    used_elevated_token = false;
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

    HandleGuard linked_token;
    HANDLE source_token = user_token.handle;
    TOKEN_LINKED_TOKEN linked{};
    DWORD ret_len = 0;
    if (GetTokenInformation(user_token.handle, TokenLinkedToken, &linked, sizeof(linked), &ret_len) &&
        linked.LinkedToken) {
        linked_token.handle = linked.LinkedToken;
        source_token = linked_token.handle;
        used_elevated_token = true;
    }

    HandleGuard primary_token;
    if (!DuplicateTokenEx(source_token,
                          TOKEN_ASSIGN_PRIMARY | TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ADJUST_DEFAULT |
                              TOKEN_ADJUST_SESSIONID,
                          nullptr,
                          SecurityImpersonation,
                          TokenPrimary,
                          &primary_token.handle)) {
        err = lastError("DuplicateTokenEx");
        return false;
    }

    if (!SetTokenInformation(primary_token.handle, TokenSessionId, &session_id, sizeof(session_id))) {
        // Non-fatal: the token often already belongs to the active session.
    }

    LPVOID environment = nullptr;
    if (!CreateEnvironmentBlock(&environment, primary_token.handle, FALSE)) {
        environment = nullptr;
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.lpDesktop = const_cast<LPWSTR>(L"winsta0\\default");

    std::wstring mutable_cmd = command_line;
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

bool launchInActiveConsoleSessionElevated(const std::wstring& command_line,
                                          PROCESS_INFORMATION& process_info,
                                          bool& used_elevated_token,
                                          std::string& err) {
    ZeroMemory(&process_info, sizeof(process_info));
    used_elevated_token = false;
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

    // 取提权链接令牌(管理员完整令牌)→ 管理员清单安装器免 UAC。
    // 取不到(非管理员/UAC 关/无链接令牌)则回退受限令牌。
    HandleGuard linked_token;
    HANDLE source_token = user_token.handle;
    TOKEN_LINKED_TOKEN linked{};
    DWORD ret_len = 0;
    if (GetTokenInformation(user_token.handle, TokenLinkedToken, &linked, sizeof(linked), &ret_len) &&
        linked.LinkedToken) {
        linked_token.handle = linked.LinkedToken;
        source_token = linked_token.handle;
        used_elevated_token = true;
    }

    HandleGuard primary_token;
    if (!DuplicateTokenEx(source_token,
                          TOKEN_ASSIGN_PRIMARY | TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ADJUST_DEFAULT |
                              TOKEN_ADJUST_SESSIONID,
                          nullptr,
                          SecurityImpersonation,
                          TokenPrimary,
                          &primary_token.handle)) {
        err = lastError("DuplicateTokenEx");
        return false;
    }

    // 链接令牌可能不带正确会话号 → 显式钉到活动控制台会话,确保窗口落 session 1。
    if (!SetTokenInformation(primary_token.handle, TokenSessionId, &session_id, sizeof(session_id))) {
        // 非致命:多数情况下令牌已是该会话;失败仅记不阻断。
    }

    LPVOID environment = nullptr;
    if (!CreateEnvironmentBlock(&environment, primary_token.handle, FALSE)) {
        environment = nullptr;
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.lpDesktop = const_cast<LPWSTR>(L"winsta0\\default");

    std::wstring mutable_cmd = command_line;
    // 交互模式:不加 CREATE_NO_WINDOW,让安装器向导/对话框在活动会话桌面可见可点(VNC)。
    DWORD flags = CREATE_UNICODE_ENVIRONMENT;
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

bool launchApplicationInActiveConsoleSessionElevated(const std::wstring& application_path,
                                                     const std::wstring& arguments,
                                                     const std::wstring& working_directory,
                                                     PROCESS_INFORMATION& process_info,
                                                     bool& used_elevated_token,
                                                     std::string& diagnostics,
                                                     std::string& err) {
    ZeroMemory(&process_info, sizeof(process_info));
    used_elevated_token = false;
    diagnostics.clear();

    DWORD session_id = WTSGetActiveConsoleSessionId();
    if (session_id == 0xffffffff) {
        err = "no active console session";
        return false;
    }
    std::ostringstream diag;
    diag << "active_session=" << session_id;

    HandleGuard user_token;
    if (!WTSQueryUserToken(session_id, &user_token.handle)) {
        err = lastError("WTSQueryUserToken");
        diagnostics = diag.str();
        return false;
    }
    diag << " " << tokenSessionDiagnostic(user_token.handle, "user_token_session");

    HandleGuard linked_token;
    HANDLE source_token = user_token.handle;
    TOKEN_LINKED_TOKEN linked{};
    DWORD ret_len = 0;
    if (GetTokenInformation(user_token.handle, TokenLinkedToken, &linked, sizeof(linked), &ret_len) &&
        linked.LinkedToken) {
        linked_token.handle = linked.LinkedToken;
        source_token = linked_token.handle;
        used_elevated_token = true;
        diag << " linked_token=yes " << tokenSessionDiagnostic(linked_token.handle, "linked_token_session");
    } else {
        diag << " linked_token=no error=" << GetLastError();
    }

    HandleGuard primary_token;
    if (!DuplicateTokenEx(source_token,
                          TOKEN_ASSIGN_PRIMARY | TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ADJUST_DEFAULT |
                              TOKEN_ADJUST_SESSIONID,
                          nullptr,
                          SecurityImpersonation,
                          TokenPrimary,
                          &primary_token.handle)) {
        err = lastError("DuplicateTokenEx");
        diagnostics = diag.str();
        return false;
    }

    diag << " " << tokenSessionDiagnostic(primary_token.handle, "primary_before_session");
    if (!SetTokenInformation(primary_token.handle, TokenSessionId, &session_id, sizeof(session_id))) {
        diag << " set_session=error:" << GetLastError();
    } else {
        diag << " set_session=ok";
    }
    diag << " " << tokenSessionDiagnostic(primary_token.handle, "primary_after_session");

    LPVOID environment = nullptr;
    if (!CreateEnvironmentBlock(&environment, primary_token.handle, FALSE)) {
        diag << " env=error:" << GetLastError();
        environment = nullptr;
    } else {
        diag << " env=ok";
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.lpDesktop = const_cast<LPWSTR>(L"winsta0\\default");

    std::wstring mutable_cmd = L"\"" + application_path + L"\"";
    if (!arguments.empty()) {
        mutable_cmd += L" ";
        mutable_cmd += arguments;
    }
    DWORD flags = CREATE_UNICODE_ENVIRONMENT;
    const wchar_t* cwd = working_directory.empty() ? nullptr : working_directory.c_str();
    BOOL ok = CreateProcessAsUserW(primary_token.handle,
                                   application_path.c_str(),
                                   mutable_cmd.data(),
                                   nullptr,
                                   nullptr,
                                   FALSE,
                                   flags,
                                   environment,
                                   cwd,
                                   &si,
                                   &process_info);
    if (environment) {
        DestroyEnvironmentBlock(environment);
    }
    diagnostics = diag.str();
    if (!ok) {
        err = lastError("CreateProcessAsUserW");
        return false;
    }
    return true;
}

}  // namespace device_agent::remotedesktop::windows

#endif  // _WIN32
