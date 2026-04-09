#pragma once

#include <memory>
#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>

#include "logger/logger.h"

namespace device_agent {

// Forward declarations
class IBusinessHandler;

// ─── IBusinessHandler ───────────────────────────────────────
// 业务应用实现此接口，注入业务数据和接收指令
//
// device-agent 通过 Bridge（Socket/HTTP/Plugin）调用业务应用
// 业务应用实现此接口后注册到 Bridge
class IBusinessHandler {
public:
    virtual ~IBusinessHandler() = default;

    // 获取业务指标（device-agent 定期调用）
    // 返回 JSON 字符串，格式自由，由业务定义
    // 例如：{"transactions_today": 42, "inventory_alert": false}
    virtual std::string get_business_metrics() = 0;

    // 获取业务状态（拼接到设备状态一起上报）
    // 返回 JSON 字符串
    // 例如：{"printer_status": "ok", "medicine_slots": 20}
    virtual std::string get_business_status() = 0;

    // 执行业务指令（服务端下发的自定义指令）
    // command_type: 指令类型（reboot/config_push 等标准指令由 agent 处理）
    // payload_json: 指令参数
    // 返回 true 表示成功，false 填充 error_message
    virtual bool execute_business_command(
        const std::string& command_type,
        const std::string& payload_json,
        std::string& error_message) = 0;

    // 业务应用就绪回调（Bridge 连接建立后调用）
    virtual void on_ready() = 0;
};

// ─── IBusinessBridge ───────────────────────────────────────
// 业务 Bridge 接口（Socket/HTTP/Plugin 实现）
//
// device-agent 通过此接口操作 Bridge：
//   - 启动/停止 Bridge
//   - 注册业务处理器
//   - 获取最新业务数据
class IBusinessBridge {
public:
    virtual ~IBusinessBridge() = default;

    // 启动 Bridge
    virtual bool start() = 0;

    // 停止 Bridge
    virtual void stop() = 0;

    // 注册业务处理器
    virtual void set_handler(std::shared_ptr<IBusinessHandler> handler) = 0;

    // 获取最新业务指标（非阻塞，返回空字符串表示无可用数据）
    virtual std::string poll_metrics() = 0;

    // 获取最新业务状态
    virtual std::string poll_status() = 0;

    // Bridge 类型标识
    virtual const char* type() const = 0;

    // 是否已连接（业务应用已连接）
    virtual bool is_connected() const = 0;
};

// ─── SocketBridge ─────────────────────────────────────────
// Unix Domain Socket / TCP Bridge 实现
//
// Linux/Mac: Unix Domain Socket
// Windows: TCP localhost
class SocketBridge : public IBusinessBridge {
public:
    // config: socket 路径 (Linux/Mac) 或 "host:port" (Windows)
    explicit SocketBridge(const std::string& config);
    ~SocketBridge() override;

    bool start() override;
    void stop() override;
    void set_handler(std::shared_ptr<IBusinessHandler> handler) override;
    std::string poll_metrics() override;
    std::string poll_status() override;
    const char* type() const override { return "socket"; }
    bool is_connected() const override { return connected_.load(); }

private:
    void server_loop();      // 监听 socket
    void client_loop();      // 主动连接业务应用
    void handle_message(const std::string& json);
    void send_to_client(const std::string& json);
    bool setup_server_socket();
    bool setup_client_socket();

    std::string config_;  // socket path or host:port
    std::shared_ptr<IBusinessHandler> handler_;
    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    std::atomic<bool> is_server_{false};

    int server_fd_ = -1;   // Unix socket fd / TCP listen fd
    int client_fd_ = -1;   // Unix socket fd / TCP client fd

    std::thread server_thread_;
    std::thread client_thread_;

    std::mutex metrics_mu_;
    std::mutex status_mu_;
    std::string latest_metrics_;
    std::string latest_status_;

    std::mutex send_mu_;  // 保护 send_to_client
};

// ─── NullBridge ────────────────────────────────────────────
// 空 Bridge（无业务数据注入，只有系统指标）
class NullBridge : public IBusinessBridge {
public:
    NullBridge() = default;
    bool start() override { return true; }
    void stop() override {}
    void set_handler(std::shared_ptr<IBusinessHandler>) override {}
    std::string poll_metrics() override { return "{}"; }
    std::string poll_status() override { return "{}"; }
    const char* type() const override { return "null"; }
    bool is_connected() const override { return true; }
};

}  // namespace device_agent
