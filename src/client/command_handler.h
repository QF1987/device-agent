// ============================================================
// client/command_handler.h - 指令处理
// ============================================================
// 指令处理模块，负责：
//   1. 接收来自服务端的指令（通过 DeviceClient 的回调）
//   2. 调度给具体的执行器（ICommandExecutor）
//   3. 回报执行结果给服务端
//
// 指令类型：
//   - 标准指令（reboot/config_push/upgrade_firmware）：device-agent 直接处理
//   - 自定义指令（custom）：转发给业务应用处理
//
// 异步 vs 同步执行：
//   - handle()：异步，指令立即返回，结果通过回调回报
//   - execute_sync()：同步，等待执行完成再返回
//
// 使用示例：
//   CommandHandler handler([](const CommandResult& r) {
//       return client->report_command_result(r);
//   });
//   handler.set_executor(std::make_shared<DefaultCommandExecutor>());
//   handler.handle(cmd);  // 异步执行
// ============================================================

#pragma once

#include <functional>
#include <string>
#include <memory>
#include <mutex>

#include "terminal_agent/v1/service.pb.h"
#include "terminal_agent/v1/device.pb.h"

namespace device_agent {

// ─── ICommandExecutor：指令执行器接口 ──────────────────────
// 设备端实现此接口来处理不同类型的指令。
// 这样设计：CommandHandler 只管调度，具体执行逻辑由实现类负责。
// DefaultCommandExecutor 是默认的空实现，设备厂商可继承重写。
class ICommandExecutor {
public:
    virtual ~ICommandExecutor() = default;

    // 执行指令
    //   command_type：指令类型（如 "reboot"、"custom"）
    //   payload_json：指令参数字符串（JSON 格式）
    //   timeout_seconds：超时时间（秒）
    //   error_message：失败时填充错误描述
    // 返回：true=成功，false=失败（看 error_message）
    virtual bool execute(
        const std::string& command_type,
        const std::string& payload_json,
        int64_t timeout_seconds,
        std::string& error_message) = 0;
};

// ─── CommandHandler：指令处理器 ────────────────────────────
// 接收指令、调度执行、回报结果
class CommandHandler {
public:
    // ResultReporter：结果回报函数
    // CommandResult 包含指令ID、执行结果、错误信息等
    // 返回 true 表示回报成功，false 表示需要重试
    using ResultReporter = std::function<bool(const terminal_agent::v1::CommandResult&)>;

    explicit CommandHandler(ResultReporter reporter);

    // 设置执行器（可选，不设置则用默认空执行器）
    void set_executor(std::shared_ptr<ICommandExecutor> executor);

    // 处理指令（异步模式，立即返回，结果通过回调回报）
    //   1. 根据 command_type 分发到对应 handler
    //   2. 标准指令（reboot 等）直接执行
    //   3. 自定义指令（custom）转发给 executor
    //   4. 执行完成后调用 reporter_ 回报结果
    void handle(const terminal_agent::v1::Command& cmd);

    // 同步执行指令（带超时）
    //   调用者阻塞等待，直到指令执行完成或超时
    //   返回 CommandResult（包含执行结果和错误信息）
    terminal_agent::v1::CommandResult execute_sync(
        const terminal_agent::v1::Command& cmd,
        int64_t timeout_seconds);

private:
    ResultReporter reporter_;  // 回报函数（DeviceClient::report_command_result）
    std::shared_ptr<ICommandExecutor> executor_;  // 执行器
    std::mutex mu_;  // 保护 executor_（虽然一般不并发设置）
};

// ─── DefaultCommandExecutor：默认执行器 ───────────────────
// 空实现，所有指令都返回"未实现"。
// 设备厂商应该继承此类，重写 execute() 来处理自定义指令。
// 标准指令（reboot 等）由 CommandHandler 内部处理，不需要在这里实现。
class DefaultCommandExecutor : public ICommandExecutor {
public:
    bool execute(
        const std::string& command_type,
        const std::string& payload_json,
        int64_t timeout_seconds,
        std::string& error_message) override;

    // 注册自定义处理器
    // 例如：register_handler("print_receipt",
    //     [](const std::string& payload, std::string& err) {
    //         // 处理 print_receipt 指令
    //         return true;
    //     });
    void register_handler(
        const std::string& command_type,
        std::function<bool(const std::string& payload, std::string& err)> handler);
};

}  // namespace device_agent
