// ============================================================
// executor/executor.h - 平台执行器抽象接口
// ============================================================
// 定义所有平台执行器的抽象接口。
// device-agent 核心只调用此接口，不涉及任何平台细节。
//
// 设计原则：
//   - 纯虚接口，所有方法返回 void 或 bool
//   - 错误信息通过 out 参数返回（不抛异常）
//   - Android 特有功能（如 upgradeApp）在 AndroidExecutor 中单独实现
//
// 使用方式：
//   CommandHandler 持有 std::shared_ptr<Executor>
//   平台启动时注入具体的 Executor 实现（LinuxExecutor / AndroidExecutor）
// ============================================================

#pragma once

#include <string>

namespace device_agent {

// ─── Executor：平台执行器抽象接口 ─────────────────────────
// 所有平台执行器必须实现此接口。
// device-agent 核心通过此接口执行系统操作，不直接调用系统 API。
class Executor {
public:
    virtual ~Executor() = default;

    // reboot：重启设备
    //   force=true 表示强制重启（不等进程优雅退出）
    //   command_id：指令 ID，用于 pending 状态管理
    //   err：失败时填充错误描述
    // 返回：成功返回 "pending"（reboot 已计划），失败返回 "failed"
    virtual std::string reboot(bool force, const std::string& command_id, std::string& err) = 0;

    // updateConfig：更新配置项
    //   key：配置项名称
    //   value：新的配置值
    //   err：失败时填充错误描述
    virtual void updateConfig(const std::string& key, const std::string& value, std::string& err) = 0;

    // upgradeFirmware：升级固件
    //   url：固件包下载地址（HTTP/HTTPS）
    //   md5：固件包 MD5 校验值（可选，空字符串表示不校验）
    //   err：失败时填充错误描述
    virtual void upgradeFirmware(const std::string& url, const std::string& md5, std::string& err) = 0;

    // upgradeApp：升级 Android 应用
    //   apkUrl：APK 下载地址
    //   md5：APK MD5 校验值（可选，空字符串表示不校验）
    //   err：失败时填充错误描述
    // 注意：LinuxExecutor 此方法永远返回 "not supported"
    virtual void upgradeApp(const std::string& apkUrl, const std::string& md5, std::string& err) = 0;
};

// ─── LinuxExecutor：Linux/macOS 实现 ─────────────────────
// 在 Linux/macOS 上运行的默认执行器。
class LinuxExecutor : public Executor {
public:
    std::string reboot(bool force, const std::string& command_id, std::string& err) override;
    void updateConfig(const std::string& key, const std::string& value, std::string& err) override;
    void upgradeFirmware(const std::string& url, const std::string& md5, std::string& err) override;
    void upgradeApp(const std::string& apkUrl, const std::string& md5, std::string& err) override;
};

// ─── MacOSExecutor：macOS 专用执行器 ─────────────────────
// 继承 LinuxExecutor，重写 macOS 特有行为：
//   - reboot：fork + /sbin/reboot（真正重启）
//   - upgradeApp：cp -R .app 到 /Applications
class MacOSExecutor : public Executor {
public:
    std::string reboot(bool force, const std::string& command_id, std::string& err) override;
    void updateConfig(const std::string& key, const std::string& value, std::string& err) override;
    void upgradeFirmware(const std::string& url, const std::string& md5, std::string& err) override;
    void upgradeApp(const std::string& appPath, const std::string& md5, std::string& err) override;
};

}  // namespace device_agent
