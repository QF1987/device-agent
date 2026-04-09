#include "bridge/bridge.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#endif

#include <cstring>
#include <sstream>
#include <iostream>
#include <chrono>

namespace device_agent {

// ─── SocketBridge ─────────────────────────────────────────

SocketBridge::SocketBridge(const std::string& config)
    : config_(config) {}

SocketBridge::~SocketBridge() {
    stop();
}

bool SocketBridge::start() {
    if (running_.exchange(true)) {
        return true;
    }

    // 判断是 Unix socket 还是 TCP
    if (config_.find(':') != std::string::npos) {
        // TCP: "host:port" format
        is_server_ = false;  // client mode
        client_thread_ = std::thread(&SocketBridge::client_loop, this);
    } else {
        // Unix domain socket - start server
        is_server_ = true;
        if (!setup_server_socket()) {
            running_ = false;
            return false;
        }
        server_thread_ = std::thread(&SocketBridge::server_loop, this);
    }

    LOG_INFO("SocketBridge started: " + config_);
    return true;
}

void SocketBridge::stop() {
    if (!running_.exchange(false)) {
        return;
    }

    if (is_server_) {
#ifdef _WIN32
        if (server_fd_ != -1) closesocket(server_fd_);
        if (client_fd_ != -1) closesocket(client_fd_);
#else
        if (server_fd_ != -1) close(server_fd_);
        if (client_fd_ != -1) close(client_fd_);
#endif
    } else {
#ifdef _WIN32
        if (client_fd_ != -1) closesocket(client_fd_);
#else
        if (client_fd_ != -1) close(client_fd_);
#endif
    }

    if (server_thread_.joinable()) server_thread_.join();
    if (client_thread_.joinable()) client_thread_.join();

    connected_.store(false);
    LOG_INFO("SocketBridge stopped");
}

void SocketBridge::set_handler(std::shared_ptr<IBusinessHandler> handler) {
    handler_ = handler;
}

std::string SocketBridge::poll_metrics() {
    std::lock_guard<std::mutex> lock(metrics_mu_);
    std::string ret = latest_metrics_;
    latest_metrics_.clear();
    return ret;
}

std::string SocketBridge::poll_status() {
    std::lock_guard<std::mutex> lock(status_mu_);
    std::string ret = latest_status_;
    latest_status_.clear();
    return ret;
}

bool SocketBridge::setup_server_socket() {
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        LOG_ERROR("WSAStartup failed");
        return false;
    }

    server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd_ == -1) {
        LOG_ERROR("socket() failed");
        return false;
    }

    // Parse host:port
    size_t colon = config_.find(':');
    std::string host = config_.substr(0, colon);
    int port = std::stoi(config_.substr(colon + 1));

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    int opt = 1;
    setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    if (bind(server_fd_, (sockaddr*)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("bind() failed: " + std::to_string(errno));
        return false;
    }
#else
    server_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd_ == -1) {
        LOG_ERROR("socket() failed");
        return false;
    }

    // Remove existing socket file
    unlink(config_.c_str());

    sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, config_.c_str(), sizeof(addr.sun_path) - 1);

    if (bind(server_fd_, (sockaddr*)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("bind() failed: " + config_);
        return false;
    }
#endif

    if (listen(server_fd_, 5) < 0) {
        LOG_ERROR("listen() failed");
        return false;
    }

    return true;
}

void SocketBridge::server_loop() {
    LOG_INFO("SocketBridge server loop started");

    while (running_) {
#ifdef _WIN32
        u_long mode = 1;
        ioctlsocket(server_fd_, FIONBIO, &mode);

        sockaddr_in client_addr;
        int client_len = sizeof(client_addr);
        client_fd_ = accept(server_fd_, (sockaddr*)&client_addr, &client_len);
#else
        int flags = fcntl(server_fd_, F_GETFL, 0);
        fcntl(server_fd_, F_SETFL, flags | O_NONBLOCK);

        sockaddr_un client_addr;
        socklen_t client_len = sizeof(client_addr);
        client_fd_ = accept(server_fd_, (sockaddr*)&client_addr, &client_len);
#endif

        if (client_fd_ < 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        connected_.store(true);
        LOG_INFO("Business app connected");

        // Notify handler
        if (handler_) {
            handler_->on_ready();
        }

        // Read loop
        std::string buffer;
        char buf[4096];

        while (running_) {
            int n = recv(client_fd_, buf, sizeof(buf) - 1, 0);
            if (n <= 0) {
                if (n < 0) {
                    LOG_WARN("recv() failed");
                }
                break;
            }
            buf[n] = '\0';
            buffer += buf;

            // Process complete lines (JSON lines)
            size_t pos;
            while ((pos = buffer.find('\n')) != std::string::npos) {
                std::string line = buffer.substr(0, pos);
                buffer.erase(0, pos + 1);
                handle_message(line);
            }
        }

#ifdef _WIN32
        closesocket(client_fd_);
#else
        close(client_fd_);
#endif
        client_fd_ = -1;
        connected_.store(false);
        LOG_INFO("Business app disconnected");
    }
}

void SocketBridge::client_loop() {
    LOG_INFO("SocketBridge client mode (TCP connect to business app): " + config_);

    while (running_) {
        if (!setup_client_socket()) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }

        connected_.store(true);
        LOG_INFO("Connected to business app");

        if (handler_) {
            handler_->on_ready();
        }

        // Read loop
        std::string buffer;
        char buf[4096];

        while (running_) {
            int n = recv(client_fd_, buf, sizeof(buf) - 1, 0);
            if (n <= 0) {
                break;
            }
            buf[n] = '\0';
            buffer += buf;

            size_t pos;
            while ((pos = buffer.find('\n')) != std::string::npos) {
                std::string line = buffer.substr(0, pos);
                buffer.erase(0, pos + 1);
                handle_message(line);
            }
        }

#ifdef _WIN32
        closesocket(client_fd_);
#else
        close(client_fd_);
#endif
        client_fd_ = -1;
        connected_.store(false);
        LOG_WARN("Disconnected from business app, reconnecting in 5s...");
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }
}

bool SocketBridge::setup_client_socket() {
#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);

    client_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (client_fd_ == -1) return false;

    size_t colon = config_.find(':');
    std::string host = config_.substr(0, colon);
    int port = std::stoi(config_.substr(colon + 1));

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host.c_str(), &addr.sin_addr);

    if (connect(client_fd_, (sockaddr*)&addr, sizeof(addr)) < 0) {
        closesocket(client_fd_);
        client_fd_ = -1;
        return false;
    }
#else
    client_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (client_fd_ == -1) return false;

    sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, config_.c_str(), sizeof(addr.sun_path) - 1);

    if (connect(client_fd_, (sockaddr*)&addr, sizeof(addr)) < 0) {
        close(client_fd_);
        client_fd_ = -1;
        return false;
    }
#endif

    return true;
}

void SocketBridge::handle_message(const std::string& json) {
    // Very naive JSON parsing - just extract type and data
    // For production, use nlohmann/json or similar

    auto get_str = [&](const std::string& key) -> std::string {
        std::string q = "\"" + key + "\"";
        size_t pos = json.find(q);
        if (pos == std::string::npos) return "";
        size_t colon = json.find(':', pos);
        if (colon == std::string::npos) return "";
        size_t quote = json.find('"', colon);
        if (quote == std::string::npos) return "";
        size_t end_quote = json.find('"', quote + 1);
        if (end_quote == std::string::npos) return "";
        return json.substr(quote + 1, end_quote - quote - 1);
    };

    std::string type = get_str("type");

    if (type == "metrics") {
        // Extract data field
        size_t data_pos = json.find("\"data\"");
        if (data_pos != std::string::npos) {
            size_t brace = json.find('{', data_pos);
            size_t end_brace = json.rfind('}');
            if (brace != std::string::npos && end_brace != std::string::npos) {
                std::lock_guard<std::mutex> lock(metrics_mu_);
                latest_metrics_ = json.substr(brace, end_brace - brace + 1);
            }
        }
    } else if (type == "status") {
        size_t data_pos = json.find("\"data\"");
        if (data_pos != std::string::npos) {
            size_t brace = json.find('{', data_pos);
            size_t end_brace = json.rfind('}');
            if (brace != std::string::npos && end_brace != std::string::npos) {
                std::lock_guard<std::mutex> lock(status_mu_);
                latest_status_ = json.substr(brace, end_brace - brace + 1);
            }
        }
    } else if (type == "execute_command") {
        if (!handler_) return;

        std::string id = get_str("id");
        std::string cmd_type = get_str("command_type");

        // Extract payload
        size_t payload_pos = json.find("\"payload\"");
        std::string payload = "{}";
        if (payload_pos != std::string::npos) {
            size_t brace = json.find('{', payload_pos);
            size_t end_brace = json.rfind('}');
            if (brace != std::string::npos && end_brace != std::string::npos) {
                payload = json.substr(brace, end_brace - brace + 1);
            }
        }

        std::string err;
        bool ok = handler_->execute_business_command(cmd_type, payload, err);

        // Send result back
        std::ostringstream resp;
        resp << "{\"type\":\"command_result\",\"id\":\"" << id << "\",\"success\":" << (ok ? "true" : "false");
        if (!ok) {
            resp << ",\"message\":\"" << err << "\"";
        }
        resp << "}\n";

        send_to_client(resp.str());
    }
}

void SocketBridge::send_to_client(const std::string& json) {
    if (client_fd_ < 0) return;

    std::lock_guard<std::mutex> lock(send_mu_);
    std::string msg = json + "\n";
    send(client_fd_, msg.c_str(), msg.size(), 0);
}

}  // namespace device_agent
