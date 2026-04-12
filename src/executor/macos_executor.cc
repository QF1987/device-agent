// ============================================================
// executor/macos_executor.cc - macOS 平台执行器实现
// ============================================================

#include "executor/executor.h"
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

void MacOSExecutor::reboot(bool force, std::string& err) {
    (void)force;  // macOS 上 force 参数暂未使用
    LOG_INFO("MacOSExecutor: executing reboot");

    // 测试模式：不真重启（环境变量 DEVICE_AGENT_TEST_MODE=1）
    if (std::getenv("DEVICE_AGENT_TEST_MODE") != nullptr) {
        LOG_WARN("MacOSExecutor: TEST MODE - skipping real reboot");
        return;
    }

    // fork 出子进程执行 reboot，父进程立即返回
    pid_t pid = fork();
    if (pid < 0) {
        err = "fork() failed: " + std::string(strerror(errno));
        LOG_ERROR("MacOSExecutor: fork failed: " + err);
        return;
    }

    if (pid == 0) {
        // 子进程：延迟 3 秒后执行重启
        // 延迟确保父进程能先返回结果给调用方
        sleep(3);
        sync();
        // /sbin/reboot 需要 root 权限
        // 设备通常以 root 运行，如果非 root 会有权限错误
        int ret = system("/sbin/reboot");
        if (ret != 0) {
            LOG_ERROR("MacOSExecutor: reboot failed, ret=" + std::to_string(ret));
        }
        _exit(1);  // 不会执行到这里
    }

    LOG_INFO("MacOSExecutor: reboot scheduled in 3 seconds (child pid=" + std::to_string(pid) + ")");
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
