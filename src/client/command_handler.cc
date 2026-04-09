#include "client/command_handler.h"
#include "logger/logger.h"
#include <thread>

namespace device_agent {

bool DefaultCommandExecutor::execute(
        const std::string& command_type,
        const std::string& payload_json,
        int64_t timeout_seconds,
        std::string& error_message) {
    LOG_WARN("DefaultCommandExecutor: unhandled command type: " + command_type);
    error_message = "DefaultCommandExecutor: command type '" + command_type + "' not implemented";
    return false;
}

void DefaultCommandExecutor::register_handler(
        const std::string& command_type,
        std::function<bool(const std::string& payload, std::string& err)> handler) {
    // TODO: implement handler registry
}

CommandHandler::CommandHandler(ResultReporter reporter)
    : reporter_(std::move(reporter)) {}

void CommandHandler::set_executor(std::shared_ptr<ICommandExecutor> executor) {
    std::lock_guard<std::mutex> lock(mu_);
    executor_ = std::move(executor);
}

void CommandHandler::handle(const terminal_agent::v1::Command& cmd) {
    LOG_INFO("Received command: " + cmd.command_type() + " (id: " + cmd.command_id() + ")");

    std::thread([this, cmd]() {
        auto result = execute_sync(cmd, cmd.timeout_seconds());
        reporter_(result);
    }).detach();
}

terminal_agent::v1::CommandResult CommandHandler::execute_sync(
        const terminal_agent::v1::Command& cmd,
        int64_t timeout_seconds) {

    terminal_agent::v1::CommandResult result;
    result.set_command_id(cmd.command_id());
    result.set_executed_at(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

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
        // Use default executor if none set
        DefaultCommandExecutor default_exec;
        ok = default_exec.execute(
            cmd.command_type(),
            cmd.payload_json(),
            timeout_seconds,
            err_msg);
    }

    result.set_status(ok ? "success" : "failed");
    result.set_message(ok ? "ok" : err_msg);

    return result;
}

}  // namespace device_agent
