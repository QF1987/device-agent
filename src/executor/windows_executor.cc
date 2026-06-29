#ifdef _WIN32

#include "executor/executor.h"

#include "logger/logger.h"

#include <windows.h>

#include <chrono>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <vector>
#include <thread>

namespace device_agent {

namespace {

// 硬兜底封顶：软超时再大也不超过 30 分钟（防 silent_probe_timeout_seconds 被配成超长值）。
constexpr DWORD kInstallHardCapMs = 30UL * 60UL * 1000UL;
// 软超时缺省（秒）：试静默卡交互框时据此判 NEEDS_ATTENDED。
constexpr int kInstallSoftTimeoutDefaultSec = 120;

std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) {
        return std::wstring();
    }
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (len <= 0) {
        len = MultiByteToWideChar(CP_ACP, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
        if (len <= 0) {
            return std::wstring(s.begin(), s.end());
        }
        std::wstring out(static_cast<size_t>(len), L'\0');
        MultiByteToWideChar(CP_ACP, 0, s.c_str(), static_cast<int>(s.size()), out.data(), len);
        return out;
    }
    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), len);
    return out;
}

std::string last_error_message(DWORD code) {
    LPSTR buffer = nullptr;
    const DWORD len = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        code,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPSTR>(&buffer),
        0,
        nullptr);
    std::string msg = len > 0 && buffer ? std::string(buffer, len) : "unknown error";
    if (buffer) {
        LocalFree(buffer);
    }
    while (!msg.empty() && (msg.back() == '\r' || msg.back() == '\n' || msg.back() == ' ')) {
        msg.pop_back();
    }
    return msg;
}

std::string trim_copy(const std::string& s) {
    const auto first = std::find_if_not(s.begin(), s.end(), [](unsigned char c) {
        return std::isspace(c) != 0;
    });
    const auto last = std::find_if_not(s.rbegin(), s.rend(), [](unsigned char c) {
        return std::isspace(c) != 0;
    }).base();
    if (first >= last) {
        return std::string();
    }
    return std::string(first, last);
}

std::vector<DWORD> parse_success_codes(const std::string& successCodes) {
    std::vector<DWORD> codes;
    std::stringstream ss(successCodes.empty() ? "0" : successCodes);
    std::string part;
    while (std::getline(ss, part, ',')) {
        part = trim_copy(part);
        if (part.empty()) {
            continue;
        }
        char* end = nullptr;
        const long parsed = std::strtol(part.c_str(), &end, 10);
        if (end != part.c_str() && *end == '\0' && parsed >= 0) {
            codes.push_back(static_cast<DWORD>(parsed));
        }
    }
    if (codes.empty()) {
        codes.push_back(0);
    }
    return codes;
}

std::wstring quote_arg(const std::wstring& arg) {
    std::wstring quoted = L"\"";
    for (wchar_t ch : arg) {
        if (ch == L'"') {
            quoted += L'\\';
        }
        quoted += ch;
    }
    quoted += L"\"";
    return quoted;
}

std::wstring parent_directory(const std::wstring& path) {
    const size_t pos = path.find_last_of(L"\\/");
    if (pos == std::wstring::npos) {
        return std::wstring();
    }
    return path.substr(0, pos);
}

}  // namespace

std::string WindowsExecutor::reboot(bool force, const std::string& command_id, std::string& err) {
    (void)force;
    LOG_INFO("WindowsExecutor: scheduling reboot, command_id=" + command_id);
    if (std::getenv("DEVICE_AGENT_TEST_MODE") != nullptr) {
        LOG_WARN("WindowsExecutor: TEST MODE - skipping real reboot");
        return "pending";
    }

    std::thread([]() {
        std::this_thread::sleep_for(std::chrono::seconds(3));
        ::ExitWindowsEx(EWX_REBOOT | EWX_FORCEIFHUNG, SHTDN_REASON_MAJOR_APPLICATION);
    }).detach();
    return "pending";
}

void WindowsExecutor::updateConfig(const std::string& key, const std::string& value, std::string& err) {
    (void)err;
    LOG_INFO("WindowsExecutor: updateConfig key=" + key + " value=" + value);
    const char* temp = std::getenv("TEMP");
    std::string path = temp && temp[0] ? std::string(temp) + "\\device-agent-config.bak"
                                      : "device-agent-config.bak";
    std::ofstream ofs(path);
    if (ofs.is_open()) {
        ofs << "# device-agent config\n" << key << " = " << value << "\n";
    }
}

void WindowsExecutor::upgradeFirmware(const std::string& url, const std::string& md5, std::string& err) {
    (void)md5;
    LOG_INFO("WindowsExecutor: upgradeFirmware url=" + url);
    err = "upgradeFirmware is not implemented on Windows";
}

void WindowsExecutor::upgradeApp(const std::string& appPath,
                                 const std::string& md5,
                                 const std::string& command_id,
                                 std::string& err) {
    (void)appPath;
    (void)md5;
    (void)command_id;
    err = "upgradeApp is only supported on Android";
}

InstallOutcome WindowsExecutor::installPackage(const std::string& packagePath,
                                               const std::string& args,
                                               const std::string& successCodes,
                                               int softTimeoutSeconds,
                                               const std::string& command_id,
                                               std::string& err) {
    LOG_INFO("WindowsExecutor: installPackage cmd_id=" + command_id + " path=" + packagePath);
    if (packagePath.empty()) {
        err = "package path is empty";
        return InstallOutcome::FAILED;
    }

    const DWORD attrs = GetFileAttributesW(utf8_to_wide(packagePath).c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        err = "package file not found: " + packagePath;
        LOG_ERROR("WindowsExecutor: " + err);
        return InstallOutcome::FAILED;
    }

    const std::wstring app = utf8_to_wide(packagePath);
    std::wstring cmdline = quote_arg(app);
    if (!trim_copy(args).empty()) {
        cmdline += L" ";
        cmdline += utf8_to_wide(args);
    }
    std::vector<wchar_t> mutable_cmdline(cmdline.begin(), cmdline.end());
    mutable_cmdline.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring cwd = parent_directory(app);

    const BOOL ok = CreateProcessW(
        app.c_str(),
        mutable_cmdline.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_NO_WINDOW,
        nullptr,
        cwd.empty() ? nullptr : cwd.c_str(),
        &si,
        &pi);
    if (!ok) {
        const DWORD code = GetLastError();
        err = "CreateProcess failed: " + last_error_message(code) + " (" + std::to_string(code) + ")";
        LOG_ERROR("WindowsExecutor: " + err);
        return InstallOutcome::FAILED;
    }

    // 软超时：试静默卡交互框时据此判 NEEDS_ATTENDED；硬 30min 仅作封顶兜底。
    const int soft_sec = softTimeoutSeconds > 0 ? softTimeoutSeconds
                                                : kInstallSoftTimeoutDefaultSec;
    DWORD soft_ms = (static_cast<DWORD>(soft_sec) > kInstallHardCapMs / 1000UL)
                        ? kInstallHardCapMs
                        : static_cast<DWORD>(soft_sec) * 1000UL;

    LOG_INFO("WindowsExecutor: installer started cmd_id=" + command_id +
             " soft_timeout_s=" + std::to_string(soft_ms / 1000UL));
    const DWORD wait_result = WaitForSingleObject(pi.hProcess, soft_ms);
    if (wait_result == WAIT_TIMEOUT) {
        // 软超时未拿到退出码 → 判定卡交互框 → 杀进程并转人工接管。
        // 卡点在文件释放前（诊断 §2），TerminateProcess 不会留半装残态。
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        err = "silent install timed out after " + std::to_string(soft_ms / 1000UL) +
              "s, needs attended install";
        LOG_WARN("WindowsExecutor: " + err + " cmd_id=" + command_id);
        return InstallOutcome::NEEDS_ATTENDED;
    }
    if (wait_result == WAIT_FAILED) {
        const DWORD code = GetLastError();
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        err = "WaitForSingleObject failed: " + last_error_message(code) + " (" + std::to_string(code) + ")";
        LOG_ERROR("WindowsExecutor: " + err);
        return InstallOutcome::FAILED;
    }

    DWORD exit_code = 0;
    if (!GetExitCodeProcess(pi.hProcess, &exit_code)) {
        const DWORD code = GetLastError();
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        err = "GetExitCodeProcess failed: " + last_error_message(code) + " (" + std::to_string(code) + ")";
        LOG_ERROR("WindowsExecutor: " + err);
        return InstallOutcome::FAILED;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    const auto codes = parse_success_codes(successCodes);
    if (std::find(codes.begin(), codes.end(), exit_code) == codes.end()) {
        err = "installer exit code " + std::to_string(exit_code) +
              " not in success codes: " + (successCodes.empty() ? "0" : successCodes);
        LOG_ERROR("WindowsExecutor: " + err);
        return InstallOutcome::FAILED;
    }
    LOG_INFO("WindowsExecutor: installer completed successfully exit_code=" +
             std::to_string(exit_code) + " cmd_id=" + command_id);
    return InstallOutcome::SUCCESS;
}

}  // namespace device_agent

#endif  // _WIN32
