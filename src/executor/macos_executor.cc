// ============================================================
// executor/macos_executor.cc - macOS 平台执行器实现
// ============================================================

#include "executor/executor.h"
#include "reboot_state/reboot_state.h"
#include "logger/logger.h"

#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>
#include <chrono>

namespace device_agent {

// ─── reboot ───────────────────────────────────────────────

std::string MacOSExecutor::reboot(bool force, const std::string& command_id, std::string& err) {
    (void)force;  // macOS 上 force 参数暂未使用
    LOG_INFO("MacOSExecutor: executing reboot, command_id=" + command_id);

    // 测试模式：不真重启（环境变量 DEVICE_AGENT_TEST_MODE=1）
    if (std::getenv("DEVICE_AGENT_TEST_MODE") != nullptr) {
        LOG_WARN("MacOSExecutor: TEST MODE - skipping real reboot");
        return "pending";
    }

    // ─── C+D 方案：写 pending 状态 ───────────────────────
    RebootStateManager& state_mgr = RebootStateManager::instance();
    int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    state_mgr.write_pending(command_id, "DEV-001", now_ms);

    // ─── fork 子进程执行 reboot ───────────────────────────
    pid_t pid = fork();
    if (pid < 0) {
        err = "fork() failed: " + std::string(strerror(errno));
        LOG_ERROR("MacOSExecutor: fork failed: " + err);
        return "failed";
    }

    if (pid == 0) {
        // 子进程：延迟 3 秒后执行重启
        sleep(3);
        sync();

        // ── 清 pending 文件（reboot 即将执行）─────────────
        // 如果 reboot 成功，系统重启，文件已清 → 启动时无 pending
        // 如果 reboot 失败，文件被删 → 启动时无 pending（由 failure 处理）
        state_mgr.clear_pending();

        // 执行 reboot 命令
        int ret = system("/sbin/reboot");
        // 如果 reboot 成功，进程被内核杀掉，不会执行到这里
        // 如果失败，写失败状态文件
        LOG_ERROR("MacOSExecutor: reboot command returned ret=" + std::to_string(ret) +
                  " (system did not reboot)");
        std::ofstream ofs("/tmp/device-agent-reboot-status.json");
        if (ofs.is_open()) {
            ofs << "{\n"
                << "  \"command_id\": \"" << command_id << "\",\n"
                << "  \"status\": \"failed\",\n"
                << "  \"error\": \"reboot command returned " << std::to_string(ret) << "\"\n"
                << "}\n";
            ofs.close();
        }
        _exit(1);  // reboot 失败，退出
    }

    // ─── 父进程：非阻塞等待 ──────────────────────────────
    LOG_INFO("MacOSExecutor: reboot child pid=" + std::to_string(pid));

    int status;
    pid_t waited = waitpid(pid, &status, WNOHANG);
    if (waited == 0) {
        // 子进程还在运行（reboot 可能正在执行），返回 pending
        LOG_INFO("MacOSExecutor: reboot in progress, returning pending");
        return "pending";
    }

    // ─── 子进程已退出 = reboot 失败（立即返回）─────────
    // 能执行到这里说明子进程立即退出了（如权限拒绝）
    if (WIFEXITED(status)) {
        int exit_code = WEXITSTATUS(status);
        LOG_ERROR("MacOSExecutor: reboot child exited immediately with code " + std::to_string(exit_code));
        err = "reboot failed: command returned exit code " + std::to_string(exit_code);
        return "failed";
    }

    return "pending";
}

// ─── updateConfig ──────────────────────────────────────────

void MacOSExecutor::updateConfig(const std::string& key, const std::string& value, std::string& err) {
    LOG_INFO("MacOSExecutor: updateConfig key=" + key + " value=" + value);

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

void MacOSExecutor::upgradeFirmware(const std::string& url, const std::string& md5, std::string& err) {
    LOG_INFO("MacOSExecutor: upgradeFirmware url=" + url + " md5=" + md5);

    if (url.empty()) {
        err = "firmware URL is empty";
        return;
    }

    // macOS 固件升级：模拟下载过程
    // 真正的 OTA 升级需要设备-specific 实现
    std::this_thread::sleep_for(std::chrono::seconds(2));

    LOG_INFO("Firmware upgrade simulated: " + url);
    LOG_WARN("Firmware upgrade is simulated - NOT ACTUALLY APPLIED");
}

// ─── upgradeApp ───────────────────────────────────────────

void MacOSExecutor::upgradeApp(const std::string& appPath, const std::string& md5, std::string& err) {
    LOG_INFO("MacOSExecutor: upgradeApp path=" + appPath + " md5=" + md5);

    if (appPath.empty()) {
        err = "app path is empty";
        return;
    }

    // macOS 上安装 app 的标准方式：
    // 1. 如果是 .app 目录，用 cp -R 复制到 /Applications
    // 2. 如果是 .dmg，用 hdiutil 挂载后安装
    //
    // 这里用 open 命令（会触发系统安装流程）：
    // open xxx.app - 打开应用
    // open xxx.dmg - 挂载磁盘镜像
    //
    // 更可靠的方式是复制 .app 到 /Applications：
    // cp -R "/path/to/xxx.app" /Applications/

    // 检查是否是 .app 目录
    std::string appToInstall = appPath;
    if (appPath.find(".app") != std::string::npos) {
        // 构造 /Applications 目标路径
        size_t pos = appPath.find(".app");
        std::string appName = appPath.substr(0, pos + 4);  // 包含 .app
        size_t lastSlash = appName.find_last_of('/');
        if (lastSlash != std::string::npos) {
            appName = appName.substr(lastSlash + 1);  // 只保留文件名
        }
        std::string destPath = "/Applications/" + appName;

        // 复制到 /Applications（覆盖已有版本）
        std::string cmd = "cp -R \"" + appPath + "\" \"" + destPath + "\"";
        LOG_INFO("MacOSExecutor: installing app: " + cmd);
        int ret = system(cmd.c_str());
        if (ret != 0) {
            err = "failed to copy app to /Applications, ret=" + std::to_string(ret);
            LOG_ERROR("MacOSExecutor: " + err);
            return;
        }
        LOG_INFO("MacOSExecutor: app installed to " + destPath);
    } else {
        // 非 .app 文件，尝试用 open 打开
        std::string cmd = "/usr/bin/open \"" + appPath + "\"";
        LOG_INFO("MacOSExecutor: opening: " + cmd);
        int ret = system(cmd.c_str());
        if (ret != 0) {
            err = "failed to open " + appPath + ", ret=" + std::to_string(ret);
            LOG_ERROR("MacOSExecutor: " + err);
            return;
        }
    }

    LOG_INFO("MacOSExecutor: app upgrade completed");
}

}  // namespace device_agent
