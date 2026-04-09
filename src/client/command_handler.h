#pragma once

#include <functional>
#include <string>
#include <memory>
#include <mutex>

#include "terminal_agent/v1/service.pb.h"
#include "terminal_agent/v1/device.pb.h"

namespace device_agent {

// 指令执行器接口
// 设备端实现此接口来处理不同类型的指令
class ICommandExecutor {
public:
    virtual ~ICommandExecutor() = default;

    // 执行指令，返回是否成功
    // command_type: 指令类型 (reboot/update_config/upgrade_firmware/custom)
    // payload_json: 指令参数字符串
    // timeout_seconds: 超时时间
    // error_message: 失败时的错误描述
    virtual bool execute(
        const std::string& command_type,
        const std::string& payload_json,
        int64_t timeout_seconds,
        std::string& error_message) = 0;
};

// 指令处理器
// 负责接收指令、调度执行、回报结果
class CommandHandler {
public:
    using ResultReporter = std::function<bool(const terminal_agent::v1::CommandResult&)>;

    explicit CommandHandler(ResultReporter reporter);

    // 设置指令执行器
    void set_executor(std::shared_ptr<ICommandExecutor> executor);

    // 处理收到的指令
    void handle(const terminal_agent::v1::Command& cmd);

    // 同步执行指令（带超时）
    terminal_agent::v1::CommandResult execute_sync(
        const terminal_agent::v1::Command& cmd,
        int64_t timeout_seconds);

private:
    ResultReporter reporter_;
    std::shared_ptr<ICommandExecutor> executor_;
    std::mutex mu_;
};

// 默认执行器（空实现，设备端自行继承实现）
class DefaultCommandExecutor : public ICommandExecutor {
public:
    bool execute(
        const std::string& command_type,
        const std::string& payload_json,
        int64_t timeout_seconds,
        std::string& error_message) override;

    // 注册自定义处理器
    void register_handler(
        const std::string& command_type,
        std::function<bool(const std::string& payload, std::string& err)> handler);
};

}  // namespace device_agent
