// ============================================================
// config/config.h - 配置结构定义
// ============================================================
// device-agent 的所有配置项都定义在这里。
//
// 配置来源有两种：
//   1. JSON 配置文件（Config::load）
//   2. 环境变量（Config::load_from_env）
//
// 设计思路：
//   - 所有字段都有默认值，降低配置复杂度
//   - 结构体而非类：都是 public 字段，不需要封装
//   - JSON 序列化/反序列化由实现完成（见 config.cpp）
//
// 典型配置示例（JSON）：
//   {
//     "auth": {"device_id": "DEV-001", "token": "xxx"},
//     "server": {"host": "192.168.1.100", "port": 9090, "use_tls": false},
//     "heartbeat": {"interval_seconds": 30},
//     "business_bridge": {"type": "socket", "path": "/var/run/business.sock"}
//   }
// ============================================================

#pragma once

#include <string>
#include <vector>

namespace device_agent {

// ─── 设备认证 ─────────────────────────────────────────────
struct DeviceAuth {
    std::string device_id;  // 设备唯一 ID（如 DEV-001）
    std::string token;     // 认证 Token（服务端颁发）
};

// ─── 服务端连接配置 ───────────────────────────────────────
struct ServerConfig {
    std::string host;   // 服务端地址（IP 或域名）
    int port;           // gRPC 端口
    bool use_tls;       // 是否使用 TLS 加密

    // 方便拼接地址的辅助方法
    // 返回 "host:port" 格式（gRPC 地址格式）
    std::string server_address() const;
};

// ─── 心跳配置 ─────────────────────────────────────────────
struct HeartbeatConfig {
    int interval_seconds = 30;    // 心跳间隔（秒）
    int timeout_seconds = 5;      // 单次心跳超时
    int max_retries = 3;          // 超过此次数认为连接断开
};

// ─── 指令流配置 ───────────────────────────────────────────
struct CommandStreamConfig {
    int reconnect_base_seconds = 1;   // 重连初始延迟（秒）
    int reconnect_max_seconds = 60;     // 重连最大延迟（秒）
    int command_timeout_seconds = 30;   // 接收指令超时
};

// ─── 状态上报配置 ─────────────────────────────────────────
struct StatusReportConfig {
    int interval_seconds = 300;   // 上报间隔（5分钟）
    int timeout_seconds = 10;     // 单次上报超时
};

// ─── 业务 Bridge 配置 ──────────────────────────────────────
//
// mode 选项：
//   listen：device-agent 监听 socket，业务应用连接上来（推荐/默认）
//   connect：device-agent 主动连接业务应用
struct BusinessBridgeConfig {
    std::string type = "null";  // 桥接类型：null/socket/http/plugin
    std::string path;           // socket 路径或地址
                              //   Unix socket：完整路径，如 /var/run/device-agent/business.sock
                              //   TCP：地址，如 127.0.0.1:7890
    std::string mode = "listen"; // 模式：listen（默认）或 connect
    int reconnect_interval = 5;  // 重连间隔（秒，connect 模式下使用）
};

// ─── 主配置结构 ───────────────────────────────────────────
// 聚合所有子配置，是 Config::load() 的返回值类型
struct Config {
    DeviceAuth auth;
    ServerConfig server;
    HeartbeatConfig heartbeat;
    CommandStreamConfig command_stream;
    StatusReportConfig status_report;
    BusinessBridgeConfig business_bridge;

    std::string log_level = "info";  // 日志级别：debug/info/warn/error
    std::string log_file;             // 日志文件路径（空=stdout）

    // 从 JSON 文件加载配置
    // 文件不存在或格式错误会抛异常（std::runtime_error）
    static Config load(const std::string& filepath);

    // 从环境变量加载配置（用于容器/嵌入式环境）
    // 环境变量命名惯例：DEVICE_AGENT_XXX（如 DEVICE_AGENT_DEVICE_ID）
    static Config load_from_env();
};

}  // namespace device_agent
