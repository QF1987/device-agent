// ============================================================
// client/command_handler.h - 指令处理
// ============================================================
// 指令处理模块，负责：
//   1. 接收来自服务端的指令（通过 DeviceClient 的回调）
//   2. 调度给平台执行器（Executor）执行
//   3. 回报执行结果给服务端
//
// CommandHandler 是平台无关的核心代码。
// 具体执行逻辑由 Executor 接口抽象：
//   - LinuxExecutor：Linux/macOS 实现
//   - AndroidExecutor：Android 实现（NDK/JNI）
//
// 使用示例：
//   CommandHandler handler([](const CommandResult& r) {
//       return client->report_command_result(r);
//   });
//   handler.set_executor(std::make_shared<LinuxExecutor>());
//   handler.handle(cmd);
// ============================================================

#pragma once

#include <functional>
#include <string>
#include <memory>
#include <mutex>

#include "terminal_agent/v1/service.pb.h"
#include "terminal_agent/v1/device.pb.h"

namespace device_agent {

// 前向声明：Executor 定义在 executor.h，IDownloadManager 定义在 download/idownload_manager.h
class Executor;
class IDownloadManager;
class P2PConfigStore;

// ─── CommandHandler：指令处理器 ────────────────────────────
class CommandHandler {
public:
    // ResultReporter：结果回报函数
    using ResultReporter = std::function<bool(const terminal_agent::v1::CommandResult&)>;
    using ReleaseStatusReporter =
        std::function<bool(const terminal_agent::v1::ReleaseStatusRequest&)>;

    explicit CommandHandler(ResultReporter reporter);

    // 设置平台执行器（LinuxExecutor / AndroidExecutor）
    // 如果不设置，默认用 LinuxExecutor
    void set_executor(std::shared_ptr<Executor> executor);

    // 设置下载管理器（AndroidDownloadManager / CurlDownloadManager）
    // 不设置时，download_ready 命令将返回 "not supported"
    void set_download_manager(std::shared_ptr<IDownloadManager> dl_mgr);

    // 设置 release 下载状态上报器。Android 由 Kotlin/JNI 自行上报;desktop 由 C++ 接线。
    void set_release_status_reporter(ReleaseStatusReporter reporter);

    // 设置桌面下载目录。传空时由下载管理器使用自身默认值。
    void set_download_directory(std::string directory);

    // 设置 P2P 配置存储，用于 update_config kind=p2p_seeding 热更新。
    void set_p2p_config_store(std::shared_ptr<P2PConfigStore> store);

    // 处理指令（异步模式，立即返回，结果通过回调回报）
    void handle(const terminal_agent::v1::Command& cmd);

    // 同步执行指令（带超时）
    terminal_agent::v1::CommandResult execute_sync(
        const terminal_agent::v1::Command& cmd,
        int64_t timeout_seconds);

private:
    ResultReporter reporter_;
    ReleaseStatusReporter release_status_reporter_;
    std::shared_ptr<Executor> executor_;
    std::shared_ptr<IDownloadManager> dl_mgr_;
    std::shared_ptr<P2PConfigStore> p2p_config_store_;
    std::string download_directory_;
    std::mutex mu_;
};

}  // namespace device_agent
