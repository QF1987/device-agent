// ============================================================
// client/command_handler.cc - 指令处理实现
// ============================================================
// CommandHandler 通过 Executor 接口调度执行，不含任何平台细节。
// 平台执行逻辑在 executor/ 目录下各自实现（LinuxExecutor / AndroidExecutor）。
// ============================================================

#include "client/command_handler.h"
#include "config/p2p_config_store.h"
#include "executor/executor.h"
#include "executor/installer_classifier.h"
#include "download/idownload_manager.h"
#include "logger/logger.h"

#ifdef _WIN32
#include "download/windows_download_manager.h"
#include "remotedesktop/platform/windows/windows_session_launcher.h"
#include <windows.h>
#endif

#ifdef __ANDROID__
#include "download/android_download_manager.h"
#endif

#ifdef __ANDROID__
#include "download/android_download_manager.h"
#endif

#include <cstring>
#include <cctype>
#include <cstdlib>
#include <sstream>
#include <thread>
#include <chrono>
#include <memory>
#include <algorithm>

namespace device_agent {

// ─── 工具函数 ─────────────────────────────────────────────

// 简单的 JSON 解析（提取字符串值）
static bool extract_json_string(const std::string& json, const std::string& key, std::string& out) {
    std::string q = "\"" + key + "\"";
    size_t pos = json.find(q);
    if (pos == std::string::npos) return false;

    size_t colon = json.find(':', pos);
    if (colon == std::string::npos) return false;

    size_t quote = json.find('"', colon);
    if (quote == std::string::npos) return false;

    size_t end_quote = json.find('"', quote + 1);
    if (end_quote == std::string::npos) return false;

    out = json.substr(quote + 1, end_quote - quote - 1);
    return true;
}

// 简单的 JSON 解析（提取 bool 值）
static bool extract_json_bool(const std::string& json, const std::string& key, bool& out) {
    std::string q = "\"" + key + "\"";
    size_t pos = json.find(q);
    if (pos == std::string::npos) return false;

    size_t colon = json.find(':', pos);
    if (colon == std::string::npos) return false;

    size_t start = colon + 1;
    while (start < json.size() && (json[start] == ' ' || json[start] == '\t')) start++;

    if (json.substr(start, 4) == "true") {
        out = true;
        return true;
    } else if (json.substr(start, 5) == "false") {
        out = false;
        return true;
    }
    return false;
}

static bool extract_json_int64(const std::string& json, const std::string& key, int64_t& out) {
    std::string q = "\"" + key + "\"";
    size_t pos = json.find(q);
    if (pos == std::string::npos) return false;

    size_t colon = json.find(':', pos);
    if (colon == std::string::npos) return false;

    size_t start = colon + 1;
    while (start < json.size() && std::isspace(static_cast<unsigned char>(json[start]))) {
        ++start;
    }
    size_t end = start;
    while (end < json.size()) {
        const char c = json[end];
        if (!(std::isdigit(static_cast<unsigned char>(c)) || c == '-' || c == '+')) {
            break;
        }
        ++end;
    }
    if (end == start) return false;
    const std::string token = json.substr(start, end - start);
    char* parse_end = nullptr;
    const long long parsed = std::strtoll(token.c_str(), &parse_end, 10);
    if (parse_end == token.c_str() || *parse_end != '\0') return false;
    out = static_cast<int64_t>(parsed);
    return true;
}

static terminal_agent::v1::ReleaseStatusRequest make_release_status(
        const DownloadRequest& req,
        terminal_agent::v1::ReleaseDeviceStatus status,
        int64_t downloaded_bytes,
        terminal_agent::v1::ReleaseErrorCode error_code =
            terminal_agent::v1::RELEASE_ERROR_CODE_UNSPECIFIED,
        const std::string& error_message = std::string(),
        int completion_path = 0,
        int64_t peer_bytes = 0,
        int64_t web_seed_bytes = 0) {
    terminal_agent::v1::ReleaseStatusRequest report;
    report.set_batch_id(req.batch_id);
    report.set_file_id(req.file_id);
    report.set_status(status);
    report.set_downloaded_bytes(downloaded_bytes);
    report.set_completion_path(static_cast<terminal_agent::v1::CompletionPath>(completion_path));
    report.set_peer_bytes(peer_bytes);
    report.set_web_seed_bytes(web_seed_bytes);
    report.set_error_code(error_code);
    report.set_error_message(error_message);
    report.set_timestamp(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    return report;
}

static int64_t progress_report_step_bytes(int64_t total_bytes) {
    if (total_bytes <= 0) {
        return 1024 * 1024;
    }
    const int64_t five_percent = total_bytes / 20;
    return std::max<int64_t>(five_percent, 1);
}

static void maybe_install_windows_package(
        const DownloadRequest& req,
        const std::shared_ptr<Executor>& executor,
        const CommandHandler::ReleaseStatusReporter& release_status_reporter,
        int64_t final_bytes,
        const DownloadCompletionTelemetry& telemetry,
        const std::string& action_type,
        const std::string& install_args,
        const std::string& install_success_codes,
        int silent_probe_timeout_seconds) {
#ifdef _WIN32
    // 安装意图触发判据 = 显式 action_type 或 install_args 非空。
    // 两者皆空 = 纯下载,行为不变(D5:APK/system 不受影响)。
    const bool install_requested = !action_type.empty() || !install_args.empty();
    if (!install_requested) {
        return;
    }

    // 缺省/兼容 = run_installer;其它部署动作本期未支持,留口报失败。
    const std::string effective_action =
        action_type.empty() ? std::string("run_installer") : action_type;

    auto report_install_failed = [&](terminal_agent::v1::ReleaseErrorCode error_code,
                                     const std::string& message) {
        if (release_status_reporter) {
            release_status_reporter(make_release_status(
                req,
                terminal_agent::v1::RELEASE_DEVICE_STATUS_INSTALL_FAILED,
                final_bytes,
                error_code,
                message,
                telemetry.completion_path,
                telemetry.peer_bytes,
                telemetry.web_seed_bytes));
        }
    };

    if (effective_action != "run_installer") {
        LOG_ERROR("download_ready: unsupported action_type=" + effective_action +
                  " cmd_id=" + req.command_id);
        report_install_failed(terminal_agent::v1::RELEASE_ERROR_CODE_INSTALL_ERROR,
                              "unsupported action_type: " + effective_action);
        return;
    }

    if (!executor) {
        LOG_ERROR("download_ready: install requested but executor is not configured");
        report_install_failed(terminal_agent::v1::RELEASE_ERROR_CODE_INSTALL_ERROR,
                              "executor is not configured");
        return;
    }

    const std::string package_path = windows_resolve_dest_path(req);

    // operator 未给 install_args 时,按安装包家族补默认静默开关 + 成功退出码。
    std::string effective_args = install_args;
    std::string effective_codes = install_success_codes;
    if (effective_args.empty()) {
        const InstallerFamily family = classify_installer(package_path);
        effective_args = default_silent_args(family);
        if (effective_codes.empty()) {
            effective_codes = default_success_codes(family);
        }
        LOG_INFO(std::string("download_ready: install_args empty, classified family=") +
                 installer_family_name(family) + " default_args=" + effective_args +
                 " cmd_id=" + req.command_id);
    }

    LOG_INFO("download_ready: starting silent install package=" + package_path +
             " cmd_id=" + req.command_id);
    if (release_status_reporter) {
        release_status_reporter(make_release_status(
            req,
            terminal_agent::v1::RELEASE_DEVICE_STATUS_INSTALLING,
            final_bytes,
            terminal_agent::v1::RELEASE_ERROR_CODE_UNSPECIFIED,
            "",
            telemetry.completion_path,
            telemetry.peer_bytes,
            telemetry.web_seed_bytes));
    }

    std::string install_err;
    const InstallOutcome outcome = executor->installPackage(
        package_path, effective_args, effective_codes,
        silent_probe_timeout_seconds, req.command_id, install_err);

    switch (outcome) {
        case InstallOutcome::SUCCESS:
            LOG_INFO("download_ready: silent install succeeded cmd_id=" + req.command_id);
            if (release_status_reporter) {
                release_status_reporter(make_release_status(
                    req,
                    terminal_agent::v1::RELEASE_DEVICE_STATUS_INSTALLED,
                    final_bytes,
                    terminal_agent::v1::RELEASE_ERROR_CODE_UNSPECIFIED,
                    "",
                    telemetry.completion_path,
                    telemetry.peer_bytes,
                    telemetry.web_seed_bytes));
            }
            break;
        case InstallOutcome::NEEDS_ATTENDED: {
            // 零 proto:复用 INSTALL_FAILED + BUSINESS_ERROR + "NEEDS_ATTENDED:" sentinel,
            // 由 backend(E2)识别前缀物化为 needs_attended 子态。
            const std::string sentinel =
                "NEEDS_ATTENDED: " + (install_err.empty() ? "silent probe timed out" : install_err) +
                " package=" + package_path;
            LOG_WARN("download_ready: silent install needs attended: " + sentinel);
            report_install_failed(terminal_agent::v1::RELEASE_ERROR_CODE_BUSINESS_ERROR, sentinel);
            break;
        }
        case InstallOutcome::FAILED:
        default:
            LOG_ERROR("download_ready: silent install failed: " + install_err);
            report_install_failed(terminal_agent::v1::RELEASE_ERROR_CODE_INSTALL_ERROR, install_err);
            break;
    }
#else
    (void)req;
    (void)executor;
    (void)release_status_reporter;
    (void)final_bytes;
    (void)telemetry;
    (void)action_type;
    (void)install_args;
    (void)install_success_codes;
    (void)silent_probe_timeout_seconds;
#endif
}

#ifdef _WIN32
// 退出码是否命中成功集(逗号分隔;空集按 0)。
static bool attended_exit_code_succeeded(DWORD code, const std::string& successCodes) {
    std::stringstream ss(successCodes.empty() ? "0" : successCodes);
    std::string part;
    bool any = false;
    while (std::getline(ss, part, ',')) {
        const size_t a = part.find_first_not_of(" \t");
        if (a == std::string::npos) continue;
        const size_t b = part.find_last_not_of(" \t");
        part = part.substr(a, b - a + 1);
        char* end = nullptr;
        const long v = std::strtol(part.c_str(), &end, 10);
        if (end != part.c_str() && *end == '\0' && v >= 0) {
            any = true;
            if (static_cast<DWORD>(v) == code) {
                return true;
            }
        }
    }
    return !any && code == 0;
}

// install_attended:把安装器以**交互模式**(默认去 silent 开关)用提权链接令牌拉到
// 活动会话桌面(免 UAC),供 operator 经 VNC 人工点完;等进程退出按成功码报
// INSTALLED / INSTALL_FAILED。仅 operator 显式下发触发(非自动,防 kiosk 顾客误见)。
static void run_windows_attended_install(
        const DownloadRequest& req,
        const CommandHandler::ReleaseStatusReporter& release_status_reporter,
        const std::string& attended_args,
        const std::string& install_success_codes,
        int attended_timeout_seconds,
        std::string& result_status,
        std::string& result_message) {
    namespace rdw = device_agent::remotedesktop::windows;

    const std::string package_path = windows_resolve_dest_path(req);
    const DWORD attrs = GetFileAttributesW(rdw::widen(package_path).c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        result_status = "failed";
        result_message = "package file not found: " + package_path;
        LOG_ERROR("install_attended: " + result_message);
        if (release_status_reporter) {
            release_status_reporter(make_release_status(
                req, terminal_agent::v1::RELEASE_DEVICE_STATUS_INSTALL_FAILED, 0,
                terminal_agent::v1::RELEASE_ERROR_CODE_INSTALL_ERROR, result_message));
        }
        return;
    }

    std::wstring cmdline = rdw::quoteArg(package_path);
    if (!attended_args.empty()) {
        cmdline += L" ";
        cmdline += rdw::widen(attended_args);
    }

    if (release_status_reporter) {
        release_status_reporter(make_release_status(
            req, terminal_agent::v1::RELEASE_DEVICE_STATUS_INSTALLING, 0));
    }

    PROCESS_INFORMATION pi{};
    bool used_elevated = false;
    std::string launch_err;
    if (!rdw::launchInActiveConsoleSessionElevated(cmdline, pi, used_elevated, launch_err)) {
        result_status = "failed";
        result_message = "attended launch failed: " + launch_err;
        LOG_ERROR("install_attended: " + result_message + " cmd_id=" + req.command_id);
        if (release_status_reporter) {
            release_status_reporter(make_release_status(
                req, terminal_agent::v1::RELEASE_DEVICE_STATUS_INSTALL_FAILED, 0,
                terminal_agent::v1::RELEASE_ERROR_CODE_INSTALL_ERROR, result_message));
        }
        return;
    }
    LOG_INFO(std::string("install_attended: launched in active session elevated_token=") +
             (used_elevated ? "yes" : "no") + " cmd_id=" + req.command_id);

    const DWORD timeout_ms = attended_timeout_seconds > 0
                                 ? static_cast<DWORD>(attended_timeout_seconds) * 1000UL
                                 : 3600UL * 1000UL;
    const DWORD wait_result = WaitForSingleObject(pi.hProcess, timeout_ms);
    if (wait_result == WAIT_TIMEOUT) {
        // 人工迟迟未完成:不杀(避免毁掉进行中的人工安装),保持 INSTALLING,留待人工/后续探测。
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        result_status = "success";
        result_message = "attended installer launched; still running at timeout, left to operator";
        LOG_WARN("install_attended: still running at timeout cmd_id=" + req.command_id);
        return;
    }

    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    if (attended_exit_code_succeeded(exit_code, install_success_codes)) {
        result_status = "success";
        result_message = "attended install completed exit_code=" + std::to_string(exit_code);
        LOG_INFO("install_attended: " + result_message + " cmd_id=" + req.command_id);
        if (release_status_reporter) {
            release_status_reporter(make_release_status(
                req, terminal_agent::v1::RELEASE_DEVICE_STATUS_INSTALLED, 0));
        }
    } else {
        result_status = "failed";
        result_message = "attended install exit_code=" + std::to_string(exit_code);
        LOG_ERROR("install_attended: " + result_message + " cmd_id=" + req.command_id);
        if (release_status_reporter) {
            release_status_reporter(make_release_status(
                req, terminal_agent::v1::RELEASE_DEVICE_STATUS_INSTALL_FAILED, 0,
                terminal_agent::v1::RELEASE_ERROR_CODE_INSTALL_ERROR, result_message));
        }
    }
}
#endif  // _WIN32

// ─── CommandHandler 实现 ───────────────────────────────────

CommandHandler::CommandHandler(ResultReporter reporter)
    : reporter_(std::move(reporter)), executor_(nullptr) {}

void CommandHandler::set_executor(std::shared_ptr<Executor> executor) {
    std::lock_guard<std::mutex> lock(mu_);
    executor_ = std::move(executor);
}

void CommandHandler::set_download_manager(std::shared_ptr<IDownloadManager> dl_mgr) {
    std::lock_guard<std::mutex> lock(mu_);
    dl_mgr_ = std::move(dl_mgr);
}

void CommandHandler::set_release_status_reporter(ReleaseStatusReporter reporter) {
    std::lock_guard<std::mutex> lock(mu_);
    release_status_reporter_ = std::move(reporter);
}

void CommandHandler::set_download_directory(std::string directory) {
    std::lock_guard<std::mutex> lock(mu_);
    download_directory_ = std::move(directory);
}

void CommandHandler::set_p2p_config_store(std::shared_ptr<P2PConfigStore> store) {
    std::lock_guard<std::mutex> lock(mu_);
    p2p_config_store_ = std::move(store);
}

void CommandHandler::handle(const terminal_agent::v1::Command& cmd) {
    const auto& cmd_type = cmd.command_type();
    const auto& cmd_id = cmd.command_id();

    LOG_INFO("CommandHandler: received " + cmd_type + " (id: " + cmd_id + ")");

    // 在后台线程执行（异步，不阻塞 CommandStream）
    std::thread([this, cmd]() {
        auto result = execute_sync(cmd, cmd.timeout_seconds());
        result.set_device_id("");
        reporter_(result);
    }).detach();
}

terminal_agent::v1::CommandResult CommandHandler::execute_sync(
        const terminal_agent::v1::Command& cmd,
        int64_t timeout_seconds) {

    terminal_agent::v1::CommandResult result;
    result.set_command_id(cmd.command_id());

    auto now = std::chrono::system_clock::now().time_since_epoch();
    result.set_executed_at(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());

    std::shared_ptr<Executor> executor;
    std::shared_ptr<P2PConfigStore> p2p_config_store;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!executor_) {
#ifdef __ANDROID__
            executor = std::make_shared<AndroidExecutor>();
#elif defined(_WIN32)
            LOG_INFO("No executor set, using WindowsExecutor");
            executor = std::make_shared<WindowsExecutor>();
#else
            LOG_INFO("No executor set, using LinuxExecutor");
            executor = std::make_shared<LinuxExecutor>();
#endif
        } else {
            executor = executor_;
        }
        p2p_config_store = p2p_config_store_;
    }

    const auto& cmd_type = cmd.command_type();
    const auto& payload = cmd.payload_json();
    std::string err_msg;

    if (cmd_type == "reboot") {
        bool force = false;
        extract_json_bool(payload, "force", force);
        std::string status = executor->reboot(force, cmd.command_id(), err_msg);
        result.set_status(status);
        result.set_message(err_msg.empty() ? "reboot scheduled" : err_msg);

    } else if (cmd_type == "update_config") {
        std::string kind;
        if (extract_json_string(payload, "kind", kind) && kind == "p2p_seeding") {
            if (!p2p_config_store) {
                err_msg = "p2p config store not configured";
            } else {
                std::string apply_error;
                if (!p2p_config_store->apply(payload, &apply_error)) {
                    err_msg = apply_error.empty() ? "invalid p2p config payload" : apply_error;
                }
            }
        } else {
            std::string key, value;
            if (!extract_json_string(payload, "key", key)) {
                err_msg = "invalid payload: missing 'key'";
            } else if (!extract_json_string(payload, "value", value)) {
                err_msg = "invalid payload: missing 'value'";
            } else {
                executor->updateConfig(key, value, err_msg);
            }
        }
        result.set_status(err_msg.empty() ? "success" : "failed");
        result.set_message(err_msg.empty() ? "config updated" : err_msg);

    } else if (cmd_type == "upgrade_firmware") {
        std::string target_version, url, md5;
        if (!extract_json_string(payload, "target_version", target_version)) {
            err_msg = "invalid payload: missing 'target_version'";
        } else {
            extract_json_string(payload, "url", url);
            extract_json_string(payload, "md5", md5);
            executor->upgradeFirmware(url, md5, err_msg);
        }
        result.set_status(err_msg.empty() ? "success" : "failed");
        result.set_message(err_msg.empty() ? "firmware upgrade scheduled" : err_msg);

    } else if (cmd_type == "upgrade_app") {
        std::string apkPath, md5;
        extract_json_string(payload, "apk_path", apkPath);
        extract_json_string(payload, "md5", md5);
        LOG_INFO("CommandHandler: dispatching upgrade_app cmd_id=" + cmd.command_id());
        executor->upgradeApp(apkPath, md5, cmd.command_id(), err_msg);
        result.set_status(err_msg.empty() ? "success" : "failed");
        result.set_message(err_msg.empty() ? "app upgrade scheduled" : err_msg);

    } else if (cmd_type == "download_ready") {
        std::string batch_id, file_id, file_type, url, torrent_url, magnet_uri, sha256;
        std::string action_type, install_args, install_success_codes;
        int64_t file_size = 0;
        int64_t silent_probe_timeout_seconds = 120;  // 软超时缺省 120s
        extract_json_string(payload, "batch_id", batch_id);
        extract_json_string(payload, "file_id", file_id);
        extract_json_string(payload, "file_type", file_type);
        extract_json_string(payload, "download_url", url);
        extract_json_string(payload, "torrent_url", torrent_url);
        extract_json_string(payload, "magnet_uri", magnet_uri);
        extract_json_string(payload, "sha256", sha256);
        extract_json_string(payload, "action_type", action_type);
        extract_json_string(payload, "install_args", install_args);
        extract_json_string(payload, "install_success_codes", install_success_codes);
        extract_json_int64(payload, "silent_probe_timeout_seconds", silent_probe_timeout_seconds);
        extract_json_int64(payload, "file_size", file_size);

        // 获取或创建 DownloadManager（与 Executor 一致的 fallback 设计）
        std::shared_ptr<IDownloadManager> dl_mgr;
        ReleaseStatusReporter release_status_reporter;
        std::string download_directory;
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (!dl_mgr_) {
#ifdef __ANDROID__
                dl_mgr_ = std::make_shared<AndroidDownloadManager>();
#endif
            }
            dl_mgr = dl_mgr_;
            release_status_reporter = release_status_reporter_;
            download_directory = download_directory_;
        }

        if (batch_id.empty() || file_id.empty() ||
                (url.empty() && torrent_url.empty() && magnet_uri.empty())) {
            err_msg = "invalid payload: missing required fields for download_ready";
            result.set_status("failed");
            result.set_message(err_msg);
        } else if (!dl_mgr) {
            err_msg = "no download manager configured";
            result.set_status("failed");
            result.set_message(err_msg);
        } else {
            DownloadRequest req;
            req.url = url;
            req.torrent_url = torrent_url;
            req.magnet_uri = magnet_uri;
            req.dest_path = download_directory;
            req.expected_sha256 = sha256;
            req.file_size = file_size;
            req.batch_id = batch_id;
            req.file_id = file_id;
            req.command_id = cmd.command_id();
            req.file_type = file_type;

#ifdef __ANDROID__
            if (p2p_config_store && !url.empty()) {
                const auto p2p_cfg = p2p_config_store->snapshot();
                const int64_t min_bytes =
                    static_cast<int64_t>(p2p_cfg.min_file_size_mb_for_p2p) * 1024 * 1024;
                const bool below_min_size =
                    p2p_cfg.min_file_size_mb_for_p2p > 0 &&
                    req.file_size > 0 &&
                    req.file_size < min_bytes;
                if (!p2p_cfg.p2p_enabled || below_min_size) {
                    LOG_INFO("CommandHandler: P2P policy selected HTTP download path cmd_id=" +
                             cmd.command_id());
                    dl_mgr = std::make_shared<AndroidDownloadManager>();
                }
            }
#endif

            LOG_INFO("CommandHandler: dispatching download_ready cmd_id=" + cmd.command_id() +
                     " batch=" + batch_id + " file=" + file_id);

            const auto last_downloaded_bytes = std::make_shared<int64_t>(0);
            const auto last_reported_bytes = std::make_shared<int64_t>(0);
            if (release_status_reporter) {
                release_status_reporter(make_release_status(
                    req, terminal_agent::v1::RELEASE_DEVICE_STATUS_DOWNLOADING, 0));
            }

            dl_mgr->download(
                req,
                [req, release_status_reporter, last_downloaded_bytes, last_reported_bytes](
                        const DownloadProgress& progress) {
                    *last_downloaded_bytes = progress.downloaded_bytes;
                    if (release_status_reporter) {
                        const int64_t step = progress_report_step_bytes(progress.total_bytes);
                        const bool completed =
                            progress.total_bytes > 0 &&
                            progress.downloaded_bytes >= progress.total_bytes;
                        if (!completed && progress.downloaded_bytes - *last_reported_bytes < step) {
                            return;
                        }
                        *last_reported_bytes = progress.downloaded_bytes;
                        release_status_reporter(make_release_status(
                            req,
                            terminal_agent::v1::RELEASE_DEVICE_STATUS_DOWNLOADING,
                            progress.downloaded_bytes));
                    }
                },
                [req, executor, release_status_reporter, last_downloaded_bytes,
                 action_type, install_args, install_success_codes,
                 silent_probe_timeout_seconds](
                        bool ok,
                        const std::string& err,
                        const DownloadCompletionTelemetry& telemetry) {
                    if (!ok) {
                        LOG_ERROR("download_ready: download failed: " + err);
                        if (release_status_reporter) {
                            release_status_reporter(make_release_status(
                                req,
                                terminal_agent::v1::RELEASE_DEVICE_STATUS_DOWNLOAD_FAILED,
                                *last_downloaded_bytes,
                                terminal_agent::v1::RELEASE_ERROR_CODE_NETWORK_ERROR,
                                err));
                        }
                    } else {
                        LOG_INFO("download_ready: download dispatched");
                        int64_t final_bytes = *last_downloaded_bytes;
                        if (final_bytes <= 0 && req.file_size > 0) {
                            final_bytes = req.file_size;
                        }
                        if (release_status_reporter) {
                            release_status_reporter(make_release_status(
                                req,
                                terminal_agent::v1::RELEASE_DEVICE_STATUS_DOWNLOADED,
                                final_bytes,
                                terminal_agent::v1::RELEASE_ERROR_CODE_UNSPECIFIED,
                                "",
                                telemetry.completion_path,
                                telemetry.peer_bytes,
                                telemetry.web_seed_bytes));
                        }
                        maybe_install_windows_package(
                            req,
                            executor,
                            release_status_reporter,
                            final_bytes,
                            telemetry,
                            action_type,
                            install_args,
                            install_success_codes,
                            static_cast<int>(silent_probe_timeout_seconds));
                    }
                });
            result.set_status("success");
            result.set_message("download dispatched");
        }

    } else if (cmd_type == "install_attended") {
#ifdef _WIN32
        std::string batch_id, file_id, file_type, attended_args, install_success_codes;
        int64_t attended_timeout_seconds = 3600;  // 人工交互安装默认上限 1 小时
        extract_json_string(payload, "batch_id", batch_id);
        extract_json_string(payload, "file_id", file_id);
        extract_json_string(payload, "file_type", file_type);
        // 交互模式默认裸跑(去 silent 开关);operator 如显式给 install_args 则附加。
        extract_json_string(payload, "install_args", attended_args);
        extract_json_string(payload, "install_success_codes", install_success_codes);
        extract_json_int64(payload, "attended_timeout_seconds", attended_timeout_seconds);

        ReleaseStatusReporter release_status_reporter;
        std::string download_directory;
        {
            std::lock_guard<std::mutex> lock(mu_);
            release_status_reporter = release_status_reporter_;
            download_directory = download_directory_;
        }
        if (batch_id.empty() || file_id.empty()) {
            err_msg = "invalid payload: missing batch_id/file_id for install_attended";
            result.set_status("failed");
            result.set_message(err_msg);
        } else {
            DownloadRequest req;
            req.dest_path = download_directory;
            req.batch_id = batch_id;
            req.file_id = file_id;
            req.command_id = cmd.command_id();
            req.file_type = file_type;
            std::string status_out, message_out;
            run_windows_attended_install(req, release_status_reporter, attended_args,
                                         install_success_codes,
                                         static_cast<int>(attended_timeout_seconds),
                                         status_out, message_out);
            result.set_status(status_out);
            result.set_message(message_out);
        }
#else
        err_msg = "install_attended is only supported on Windows";
        result.set_status("failed");
        result.set_message(err_msg);
#endif

    } else {
        err_msg = "unknown command type: " + cmd_type;
        result.set_status("failed");
        result.set_message(err_msg);
    }

    LOG_INFO("Command result: " + cmd_type + " -> " + result.status() +
             (err_msg.empty() ? "" : ": " + err_msg));

    return result;
}

}  // namespace device_agent
