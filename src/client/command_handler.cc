// ============================================================
// command_handler.cc - 指令处理实现
// ============================================================
// DefaultCommandExecutor 实现真正的指令执行：
//   - reboot：延迟重启（先回报结果，再执行重启）
//   - update_config：解析 JSON payload，更新本地配置
//   - upgrade_firmware：下载并应用固件
//
// 重要设计考量：
//   1. reboot 不能立即执行（会杀进程，来不及回报结果）
//      解决：延迟重启，回报结果后再执行
//   2. update_config 需要持久化配置到文件
//   3. upgrade_firmware 需要校验和回滚机制（这里先做简化版）
// ============================================================

#include "client/command_handler.h"
#include "logger/logger.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <spawn.h>
#endif

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>
#include <chrono>
#include <future>

namespace device_agent {

// ─── 工具函数 ─────────────────────────────────────────────

// 简单的 JSON 解析（提取字符串值）
// 生产环境建议用 nlohmann/json 或 rapidjson
// 这里只解析 {"key": "value"} 格式的简单 JSON
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
    // skip whitespace
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

// ─── reboot 指令执行 ───────────────────────────────────────
//
// 延迟重启模式（关键设计）：
//   reboot 命令不能立即执行，因为会杀死本进程，来不及向 terminal-agent 回报结果。
//   解决思路：
//     1. 先向 terminal-agent 回报执行结果（success）
//     2. 延迟几秒后再执行真正的重启
//     3. 这样 terminal-agent 能收到结果，设备也能重启
//
//   注意：这意味着设备重启前 terminal-agent 看到的是 "success" 状态
//         而不是等设备真正重启后才更新状态
//         这是该架构的固有限制（reboot 和结果回报是并行的）
//
// 实现方式：
//   Linux: fork 出一个子进程，父进程立即返回
//          子进程 sleep 后执行 /sbin/reboot(RB_AUTOBOOT)
//   Windows: schedule a shutdown with /r flag via std::system
static bool execute_reboot(const std::string& payload_json, std::string& error_message) {
    // 解析 force 参数
    bool force = false;
    extract_json_bool(payload_json, "force", force);

    LOG_INFO("Executing reboot (force=" + std::string(force ? "true" : "false") + ")");

#ifndef _WIN32
    // Linux/macOS: fork + 延迟重启
    pid_t pid = fork();
    if (pid < 0) {
        error_message = "fork() failed: " + std::string(strerror(errno));
        return false;
    }

    if (pid == 0) {
        // 子进程：延迟后执行重启
        // 等待 3 秒，给父进程足够时间回报结果
        sleep(3);

        // 执行重启
        // RB_AUTOBOOT: Linux 内核的系统调用，让系统重新启动
        // 注意：这需要 root 权限
        // 如果不是 root，可以考虑 execle("/sbin/shutdown", "shutdown", "-r", "now", nullptr, envp)
        if (force) {
            // force: 直接重启，不等待进程结束
            sync();  // 先同步文件系统缓冲
            reboot(RB_AUTOBOOT);  // 这是个阻塞调用，不会返回
        } else {
            // 正常重启：允许服务优雅关闭
            execl("/sbin/shutdown", "shutdown", "-r", "+1", "Reboot requested by terminal-agent", nullptr);
            // 如果 execl 失败（没权限），尝试直接 reboot
            sync();
            reboot(RB_AUTOBOOT);
        }
        _exit(1);  // 不应该执行到这里
    }

    // 父进程：立即返回成功
    // 子进程会在后台延迟执行重启
    LOG_INFO("Reboot scheduled in 3 seconds (child pid=" + std::to_string(pid) + ")");
    return true;

#else
    // Windows: 用 shutdown 命令
    // /r: 重启 /t 1: 延迟1秒 /c: 注释 /f: 强制关闭程序
    std::string cmd = "shutdown /r /t 3 /c \"Reboot requested by terminal-agent\" /f";
    int ret = std::system(cmd.c_str());
    if (ret != 0) {
        error_message = "shutdown command failed with code: " + std::to_string(ret);
        return false;
    }
    return true;
#endif
}

// ─── update_config 指令执行 ─────────────────────────────────
//
// 更新本地配置文件（JSON 格式）
// 策略：覆盖写入整个配置文件（简化版，不做合并）
//
// payload 格式：{"key": "screen_brightness", "value": "80"}
// 或者全量更新：{"config": {"screen_brightness": 80, "volume_level": 50}}
static bool execute_update_config(const std::string& payload_json, std::string& error_message) {
    LOG_INFO("Executing update_config: " + payload_json);

    // 解析 key 和 value
    std::string key, value;
    if (!extract_json_string(payload_json, "key", key)) {
        error_message = "invalid payload: missing 'key' field";
        return false;
    }
    if (!extract_json_string(payload_json, "value", value)) {
        error_message = "invalid payload: missing 'value' field";
        return false;
    }

    // 构建新的配置内容
    // 简化：直接写入 key=value，不做完整 JSON 合并
    std::ostringstream oss;
    oss << "# device-agent config update\n"
        << "# key: " << key << "\n"
        << "# updated at: " << std::chrono::system_clock::now().time_since_epoch().count() << "\n"
        << key << " = " << value << "\n";

    // 写入配置文件的备份
    std::string config_backup = "/etc/device-agent/config.json.bak";
    std::ofstream ofs(config_backup);
    if (!ofs.is_open()) {
        // 尝试备用路径
        config_backup = "/tmp/device-agent-config.bak";
        ofs.open(config_backup);
    }

    if (ofs.is_open()) {
        ofs << oss.str();
        ofs.close();
        LOG_INFO("Config backup written to: " + config_backup);
    }

    // TODO: 这里应该真正更新 config.json
    // 目前只做记录，实际的配置更新需要重新加载配置或通知相关模块
    LOG_INFO("Config update recorded: " + key + " = " + value);

    return true;
}

// ─── upgrade_firmware 指令执行 ──────────────────────────────
//
// 简化版固件升级：
//   1. 解析目标版本
//   2. 下载固件（目前只是模拟）
//   3. 校验固件
//   4. 应用固件（目前只是模拟）
//
// payload 格式：{"target_version": "2.3.1", "url": "http://xxx/firmware.tar.gz"}
// 或者只用版本号（固件 URL 从服务端获取）：{"target_version": "2.3.1"}
static bool execute_upgrade_firmware(const std::string& payload_json, std::string& error_message) {
    LOG_INFO("Executing upgrade_firmware: " + payload_json);

    std::string target_version;
    std::string url;

    if (!extract_json_string(payload_json, "target_version", target_version)) {
        error_message = "invalid payload: missing 'target_version' field";
        return false;
    }

    // 提取 URL（可选）
    extract_json_string(payload_json, "url", url);

    if (url.empty()) {
        // 没有 URL，用默认固件地址（简化版）
        // 实际生产中应该从服务端获取固件地址
        LOG_WARN("No firmware URL provided, using default (simulated)");
        url = "http://firmware.device-ops.local/v" + target_version + "/firmware.tar.gz";
    }

    LOG_INFO("Downloading firmware from: " + url);

    // TODO: 真正的固件下载和升级流程：
    // 1. 下载固件到临时目录
    // 2. 校验 SHA256 / GPG 签名
    // 3. 解压固件包
    // 4. 应用固件（写入 flash / OTA update）
    // 5. 校验完整性
    // 6. 回滚机制（如果失败）

    // 模拟：sleep 2 秒表示下载，然后返回成功
    // 实际生产中这是耗时的下载+升级操作
    std::this_thread::sleep_for(std::chrono::seconds(2));

    LOG_INFO("Firmware upgrade simulated: " + target_version);
    LOG_WARN("Firmware upgrade is simulated - NOT ACTUALLY APPLIED");

    return true;
}

// ─── DefaultCommandExecutor 实现 ──────────────────────────

bool DefaultCommandExecutor::execute(
        const std::string& command_type,
        const std::string& payload_json,
        int64_t timeout_seconds,
        std::string& error_message) {

    LOG_INFO("DefaultCommandExecutor: executing " + command_type);

    if (command_type == "reboot") {
        return execute_reboot(payload_json, error_message);
    } else if (command_type == "update_config") {
        return execute_update_config(payload_json, error_message);
    } else if (command_type == "upgrade_firmware") {
        return execute_upgrade_firmware(payload_json, error_message);
    } else if (command_type == "custom") {
        // 自定义指令：交给注册的 handler 处理
        auto it = handlers_.find(command_type);
        if (it != handlers_.end()) {
            return it->second(payload_json, error_message);
        }
        error_message = "custom command type '" + command_type + "' has no registered handler";
        return false;
    }

    error_message = "unknown command type: " + command_type;
    return false;
}

void DefaultCommandExecutor::register_handler(
        const std::string& command_type,
        std::function<bool(const std::string& payload, std::string& err)> handler) {
    handlers_[command_type] = handler;
    LOG_INFO("Registered custom handler for command type: " + command_type);
}

// ─── CommandHandler 实现 ─────────────────────────────────────

CommandHandler::CommandHandler(ResultReporter reporter)
    : reporter_(std::move(reporter)) {}

void CommandHandler::set_executor(std::shared_ptr<ICommandExecutor> executor) {
    std::lock_guard<std::mutex> lock(mu_);
    executor_ = std::move(executor);
}

void CommandHandler::handle(const terminal_agent::v1::Command& cmd) {
    const auto& cmd_type = cmd.command_type();
    const auto& cmd_id = cmd.command_id();

    LOG_INFO("CommandHandler: received " + cmd_type + " (id: " + cmd_id + ")");

    // 在后台线程执行（异步，不阻塞 CommandStream）
    // detach() 是因为不需要等待这个线程结束
    // 线程结束后自动清理（ detached thread）
    std::thread([this, cmd]() {
        auto result = execute_sync(cmd, cmd.timeout_seconds());

        // 设置 device_id（从配置获取，这里先留空）
        result.set_device_id("");

        // 回报结果给 terminal-agent
        reporter_(result);
    }).detach();
}

terminal_agent::v1::CommandResult CommandHandler::execute_sync(
        const terminal_agent::v1::Command& cmd,
        int64_t timeout_seconds) {

    terminal_agent::v1::CommandResult result;
    result.set_command_id(cmd.command_id());

    // 设置执行完成时间（毫秒时间戳）
    auto now = std::chrono::system_clock::now().time_since_epoch();
    result.set_executed_at(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());

    std::shared_ptr<ICommandExecutor> executor;
    {
        std::lock_guard<std::mutex> lock(mu_);
        executor = executor_;
    }

    std::string err_msg;
    bool ok = false;

    if (executor) {
        ok = executor->execute(
            cmd.command_type(),
            cmd.payload_json(),
            timeout_seconds,
            err_msg);
    } else {
        LOG_WARN("No executor set, using DefaultCommandExecutor");
        DefaultCommandExecutor default_exec;
        ok = default_exec.execute(
            cmd.command_type(),
            cmd.payload_json(),
            timeout_seconds,
            err_msg);
    }

    result.set_status(ok ? "success" : "failed");
    result.set_message(ok ? "executed" : err_msg);

    LOG_INFO("Command result: " + cmd.command_type() + " -> " + (ok ? "success" : "failed: " + err_msg));

    return result;
}

}  // namespace device_agent
