#pragma once

#include <memory>
#include <string>
#include <functional>
#include <thread>
#include <atomic>

#include <grpcpp/grpcpp.h>

#include "terminal_agent/v1/service.grpc.pb.h"
#include "terminal_agent/v1/service.pb.h"
#include "terminal_agent/v1/device.grpc.pb.h"
#include "terminal_agent/v1/device.pb.h"

#include "config/config.h"

namespace device_agent {

using CommandCallback = std::function<void(const terminal_agent::v1::Command&)>;

// gRPC 设备客户端
//
// 封装所有设备端与服务端的通信：
//   1. Heartbeat - 定期心跳
//   2. ReportStatus - 状态上报
//   3. ReportEvent - 事件上报
//   4. ReportCommandResult - 指令结果回报
//   5. CommandStream - 接收服务端指令（长连接）
//
// 用法：
//   DeviceClient client(config);
//   client.set_command_callback([](const Command& cmd) { ... });
//   client.start();  // 启动心跳和 CommandStream
//   client.stop();   // 停止
class DeviceClient {
public:
    explicit DeviceClient(const Config& config);
    ~DeviceClient();

    // 启动客户端（心跳 + CommandStream）
    // 此函数会启动后台线程，函数本身立即返回
    void start();

    // 停止客户端
    void stop();

    // 设置指令回调
    void set_command_callback(CommandCallback cb);

    // 手动上报状态
    bool report_status(const terminal_agent::v1::StatusReport& status);

    // 手动上报事件
    bool report_event(const terminal_agent::v1::EventReport& event);

    // 回报指令执行结果
    bool report_command_result(const terminal_agent::v1::CommandResult& result);

    // 连接状态
    bool is_connected() const { return connected_.load(); }

private:
    void heartbeat_loop();
    void status_report_loop();
    void command_stream_loop();
    void reconnect_command_stream();

    void set_auth_metadata(grpc::ClientContext& ctx);

    const Config& config_;

    std::unique_ptr<terminal_agent::v1::DeviceService::Stub> device_stub_;
    std::unique_ptr<terminal_agent::v1::CommandService::Stub> command_stub_;

    std::thread heartbeat_thread_;
    std::thread status_report_thread_;
    std::thread command_stream_thread_;

    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    std::atomic<bool> command_stream_running_{false};

    CommandCallback command_callback_;

    mutable std::mutex stub_mu_;  // guards stubs reconnection
};

}  // namespace device_agent
