// ============================================================
// client/command_handler.cc - 指令处理实现
// ============================================================
// CommandHandler 通过 Executor 接口调度执行，不含任何平台细节。
// 平台执行逻辑在 executor/ 目录下各自实现（LinuxExecutor / AndroidExecutor）。
// ============================================================

#include "client/command_handler.h"
#include "executor/executor.h"
#include "logger/logger.h"

#include <cstring>
#include <sstream>
#include <thread>
#include <chrono>

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

// ─── CommandHandler 实现 ───────────────────────────────────

CommandHandler::CommandHandler(ResultReporter reporter)
    : reporter_(std::move(reporter)), executor_(nullptr) {}

void CommandHandler::set_executor(std::shared_ptr<Executor> executor) {
    std::lock_guard<std::mutex> lock(mu_);
    executor_ = std::move(executor);
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
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!executor_) {
#ifdef __ANDROID__
            executor = std::make_shared<AndroidExecutor>();
#else
            LOG_INFO("No executor set, using LinuxExecutor");
            executor = std::make_shared<LinuxExecutor>();
#endif
        } else {
            executor = executor_;
        }
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
        std::string key, value;
        if (!extract_json_string(payload, "key", key)) {
            err_msg = "invalid payload: missing 'key'";
        } else if (!extract_json_string(payload, "value", value)) {
            err_msg = "invalid payload: missing 'value'";
        } else {
            executor->updateConfig(key, value, err_msg);
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
        std::string apkUrl, md5;
        extract_json_string(payload, "apk_url", apkUrl);
        extract_json_string(payload, "md5", md5);
        executor->upgradeApp(apkUrl, md5, err_msg);
        result.set_status(err_msg.empty() ? "success" : "failed");
        result.set_message(err_msg.empty() ? "app upgrade scheduled" : err_msg);

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
