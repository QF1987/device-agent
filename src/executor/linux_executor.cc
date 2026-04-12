// ============================================================
// executor/linux_executor.cc - Linux/macOS 平台执行器实现
// ============================================================
// 从 command_handler.cc 迁移过来的执行逻辑。
// LinuxExecutor 实现 Executor 接口，提供跨平台抽象的 Linux/macOS 实现。
//
// 注意：
//   reboot 需要 root 权限，普通用户执行会失败（macOS 上需要 sudo）
//   upgradeFirmware 目前是模拟实现，真正的 OTA 升级需要设备-specific 逻辑
// ============================================================

#include "executor/executor.h"
#include "reboot_state/reboot_state.h"
#include "logger/logger.h"

#ifdef __APPLE__
#include <unistd.h>
#include <sys/reboot.h>
#include <mach/mach.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <spawn.h>
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

// ─── reboot ───────────────────────────────────────────────

std::string LinuxExecutor::reboot(bool force, const std::string& command_id, std::string& err) {
    (void)force;
    LOG_INFO("LinuxExecutor: executing reboot, command_id=" + command_id);

    // ─── C+D 方案：写 pending 状态 ───────────────────────
    RebootStateManager& state_mgr = RebootStateManager::instance();
    int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    state_mgr.write_pending(command_id, "DEV-001", now_ms);

#ifndef __APPLE__
    // Linux: fork 子进程执行 reboot
    pid_t pid = fork();
    if (pid < 0) {
        err = "fork() failed: " + std::string(strerror(errno));
        LOG_ERROR("LinuxExecutor: fork failed: " + err);
        return "failed";
    }

    if (pid == 0) {
        // 子进程：延迟 3 秒后执行 reboot
        sleep(3);
        sync();

        // ── 清 pending 文件（reboot 即将执行）─────────────
        // 如果 reboot 成功，系统重启，文件已清 → 启动时无 pending
        // 如果 reboot 失败，文件被删 → 启动时无 pending（由 failure 处理）
        state_mgr.clear_pending();

        // 调用 reboot(RB_AUTOBOOT) 系统调用
        // 成功时不会返回，进程被内核重启系统
        // 失败时返回 -1，errno 被设置
        int ret = reboot(RB_AUTOBOOT);
        // 如果 reboot 成功，进程被内核杀掉，不会执行到这里
        // 如果失败，写失败状态文件
        LOG_ERROR("LinuxExecutor: reboot syscall failed, errno=" + std::to_string(errno));
        std::ofstream ofs("/tmp/device-agent-reboot-status.json");
        if (ofs.is_open()) {
            ofs << "{\n"
                << "  \"command_id\": \"" << command_id << "\",\n"
                << "  \"status\": \"failed\",\n"
                << "  \"error\": \"reboot syscall failed, errno=" << std::to_string(errno) << "\"\n"
                << "}\n";
            ofs.close();
        }
        _exit(1);  // reboot 失败，退出
    }

    // 父进程：非阻塞等待
    LOG_INFO("LinuxExecutor: reboot child pid=" + std::to_string(pid) + ", waiting...");
    int status;
    pid_t waited = waitpid(pid, &status, WNOHANG);
    if (waited == 0) {
        // 子进程还没退出，reboot 正在执行
        LOG_INFO("LinuxExecutor: reboot in progress, returning pending");
        return "pending";
    }

    // 子进程已退出 = reboot 失败
    if (WIFEXITED(status)) {
        int exit_code = WEXITSTATUS(status);
        LOG_ERROR("LinuxExecutor: reboot child exited with code " + std::to_string(exit_code));
        err = "reboot failed with exit code " + std::to_string(exit_code);
        state_mgr.clear_pending();
        return "failed";
    }

    return "pending";
#else
    // macOS 上走 MacOSExecutor，不应该调用到这里
    LOG_WARN("LinuxExecutor: called on macOS, should use MacOSExecutor");
    return "failed";
#endif
}

// ─── updateConfig ──────────────────────────────────────────

void LinuxExecutor::updateConfig(const std::string& key, const std::string& value, std::string& err) {
    LOG_INFO("LinuxExecutor: updateConfig key=" + key + " value=" + value);

    std::ostringstream oss;
    oss << "# device-agent config\n"
        << "# updated at: " << std::chrono::system_clock::now().time_since_epoch().count() << "\n"
        << key << " = " << value << "\n";

    std::string config_backup = "/tmp/device-agent-config.bak";
    std::ofstream ofs(config_backup);
    if (ofs.is_open()) {
        ofs << oss.str();
        ofs.close();
        LOG_INFO("Config backup written to: " + config_backup);
    }

    LOG_INFO("Config update recorded: " + key + " = " + value);
}

// ─── upgradeFirmware ──────────────────────────────────────

void LinuxExecutor::upgradeFirmware(const std::string& url, const std::string& md5, std::string& err) {
    LOG_INFO("LinuxExecutor: upgradeFirmware url=" + url + " md5=" + md5);

    if (url.empty()) {
        err = "firmware URL is empty";
        return;
    }

    // 模拟下载过程
    std::this_thread::sleep_for(std::chrono::seconds(2));

    LOG_INFO("Firmware upgrade simulated: " + url);
    LOG_WARN("Firmware upgrade is simulated - NOT ACTUALLY APPLIED");
}

// ─── upgradeApp ───────────────────────────────────────────

void LinuxExecutor::upgradeApp(const std::string&, const std::string&, std::string& err) {
    err = "upgradeApp is only supported on Android";
}

}  // namespace device_agent
