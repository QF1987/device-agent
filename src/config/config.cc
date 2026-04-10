#include "config/config.h"
#include "logger/logger.h"
#include <fstream>
#include <cstdlib>
#include <cstring>

namespace device_agent {

std::string ServerConfig::server_address() const {
    std::ostringstream oss;
    if (use_tls) {
        oss << "dns:///";  // dns:/// enables TLS by default in gRPC
    } else {
        oss << "dns:///";
    }
    oss << host << ":" << port;
    return oss.str();
}

static std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

Config Config::load(const std::string& filepath) {
    Config cfg;

    std::ifstream f(filepath);
    if (!f.is_open()) {
        LOG_WARN("Config file not found: " + filepath + ", using defaults");
        return cfg;
    }

    // Simple JSON parsing without external dependency
    // For production, use nlohmann/json or similar
    std::string content((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());

    // Very naive parsing - just extract key values
    // In production, use a proper JSON library

    auto get_str = [&](const std::string& key) -> std::string {
        std::string q1 = "\"" + key + "\"";
        size_t pos = content.find(q1);
        if (pos == std::string::npos) return "";
        size_t colon = content.find(':', pos);
        if (colon == std::string::npos) return "";
        size_t quote = content.find('"', colon);
        if (quote == std::string::npos) return "";
        size_t end_quote = content.find('"', quote + 1);
        if (end_quote == std::string::npos) return "";
        return content.substr(quote + 1, end_quote - quote - 1);
    };

    auto get_int = [&](const std::string& key, int def = 0) -> int {
        std::string q1 = "\"" + key + "\"";
        size_t pos = content.find(q1);
        if (pos == std::string::npos) return def;
        size_t colon = content.find(':', pos);
        if (colon == std::string::npos) return def;
        size_t start = colon + 1;
        while (start < content.size() && (content[start] == ' ' || content[start] == '\t')) start++;
        size_t end = start;
        while (end < content.size() && (isdigit(content[end]) || content[end] == '-')) end++;
        if (end == start) return def;
        return std::stoi(content.substr(start, end - start));
    };

    cfg.auth.device_id = get_str("device_id");
    cfg.auth.token = get_str("token");
    cfg.server.host = get_str("server_host");
    cfg.server.port = get_int("server_port", 9090);
    cfg.server.use_tls = get_str("use_tls") == "true";
    cfg.heartbeat.interval_seconds = get_int("heartbeat_interval", 30);
    cfg.heartbeat.timeout_seconds = get_int("heartbeat_timeout", 5);
    cfg.command_stream.reconnect_base_seconds = get_int("reconnect_base", 1);
    cfg.command_stream.reconnect_max_seconds = get_int("reconnect_max", 60);
    cfg.status_report.interval_seconds = get_int("status_interval", 300);
    cfg.log_level = get_str("log_level");
    cfg.log_file = get_str("log_file");

    // Business bridge
    cfg.business_bridge.type = get_str("business_bridge_type");
    cfg.business_bridge.path = get_str("business_bridge_path");
    cfg.business_bridge.mode = get_str("business_bridge_mode");
    cfg.business_bridge.reconnect_interval = get_int("business_bridge_reconnect_interval", 5);

    LOG_INFO("Config loaded from: " + filepath);
    return cfg;
}

Config Config::load_from_env() {
    Config cfg;

    auto env = [](const char* key) -> std::string {
        const char* val = std::getenv(key);
        return val ? std::string(val) : "";
    };

    cfg.auth.device_id = env("DEVICE_ID");
    cfg.auth.token = env("DEVICE_TOKEN");
    cfg.server.host = env("DEVICE_OPS_SERVER_HOST");
    cfg.server.port = std::atoi(env("DEVICE_OPS_SERVER_PORT").c_str());
    if (cfg.server.port == 0) cfg.server.port = 9090;
    cfg.server.use_tls = env("DEVICE_OPS_USE_TLS") == "1";
    cfg.heartbeat.interval_seconds = std::atoi(env("DEVICE_HEARTBEAT_INTERVAL").c_str());
    if (cfg.heartbeat.interval_seconds == 0) cfg.heartbeat.interval_seconds = 30;
    cfg.log_level = env("LOG_LEVEL");
    cfg.log_file = env("LOG_FILE");
    cfg.business_bridge.type = env("BUSINESS_BRIDGE_TYPE");
    cfg.business_bridge.path = env("BUSINESS_BRIDGE_PATH");
    cfg.business_bridge.mode = env("BUSINESS_BRIDGE_MODE");
    cfg.business_bridge.reconnect_interval = std::atoi(env("BUSINESS_BRIDGE_RECONNECT_INTERVAL").c_str());

    LOG_INFO("Config loaded from environment variables");
    return cfg;
}

}  // namespace device_agent
