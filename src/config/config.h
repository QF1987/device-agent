#pragma once

#include <string>
#include <vector>

namespace device_agent {

// Device identity and auth
struct DeviceAuth {
    std::string device_id;
    std::string token;
};

// Server connection
struct ServerConfig {
    std::string host;   // e.g. "192.168.1.100" or "device-ops.example.com"
    int port;           // e.g. 9090
    bool use_tls;       // whether to use TLS

    std::string server_address() const;
};

// Heartbeat settings
struct HeartbeatConfig {
    int interval_seconds = 30;       // heartbeat interval
    int timeout_seconds = 5;         // single heartbeat timeout
    int max_retries = 3;             // max consecutive failures before reconnect
};

// Command stream settings
struct CommandStreamConfig {
    int reconnect_base_seconds = 1;  // initial reconnect delay
    int reconnect_max_seconds = 60;  // max reconnect delay (exponential backoff)
    int command_timeout_seconds = 30; // timeout for receiving a command
};

// Status report settings
struct StatusReportConfig {
    int interval_seconds = 300;     // 5 minutes
    int timeout_seconds = 10;
};

// Business bridge config
struct BusinessBridgeConfig {
    std::string type = "null";  // "null", "socket", "http", "plugin"
    std::string path;           // socket path or "host:port" for TCP
    int reconnect_interval = 5; // seconds
};

// Main config structure
struct Config {
    DeviceAuth auth;
    ServerConfig server;
    HeartbeatConfig heartbeat;
    CommandStreamConfig command_stream;
    StatusReportConfig status_report;
    BusinessBridgeConfig business_bridge;

    std::string log_level = "info";  // debug, info, warn, error
    std::string log_file;             // empty = stdout only

    // Load config from JSON file
    static Config load(const std::string& filepath);

    // Load config from environment variables (for embedded / container use)
    static Config load_from_env();
};

}  // namespace device_agent
