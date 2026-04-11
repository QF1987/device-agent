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

void LinuxExecutor::reboot(bool force, std::string& err) {
    LOG_INFO("LinuxExecutor: executing reboot (force=" + std::string(force ? "true" : "false") + ")");

#ifndef __APPLE__
    pid_t pid = fork();
    if (pid < 0) {
        err = "fork() failed: " + std::string(strerror(errno));
        return;
    }

    if (pid == 0) {
        sleep(3);
        sync();
        reboot(RB_AUTOBOOT);
        _exit(1);
    }

    LOG_INFO("Reboot scheduled in 3 seconds (child pid=" + std::to_string(pid) + ")");
#else
    // macOS: 需要 root 权限才能 reboot，这里只记录日志
    LOG_WARN("macOS reboot requires super-user, simulating...");
    std::this_thread::sleep_for(std::chrono::seconds(1));
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
