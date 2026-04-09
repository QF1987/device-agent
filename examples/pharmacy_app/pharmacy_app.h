#pragma once

#include <memory>
#include <string>
#include <cstring>
#include <iostream>
#include <thread>
#include <atomic>
#include <chrono>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#endif

namespace business {

// 自助购药机业务指标
struct PharmacyMetrics {
    int transactions_today = 0;
    int successful_transactions = 0;
    int failed_transactions = 0;
    int inventory_alert_count = 0;
    bool printer_online = true;
    bool network_healthy = true;
    std::string last_transaction_time;
};

// 自助购药机业务状态
struct PharmacyStatus {
    std::string printer_status = "ok";
    int medicine_slots_total = 20;
    int medicine_slots_available = 15;
    bool screen_touch_working = true;
    std::string payment_status = "ok";
};

// 业务应用主类
// 实现 IBusinessHandler 接口（通过 socket 发给 device-agent）
class PharmacyBusinessApp {
public:
    PharmacyBusinessApp(const std::string& socket_path)
        : socket_path_(socket_path), connected_(false), running_(false) {}

    ~PharmacyBusinessApp() { stop(); }

    bool start() {
        if (running_.exchange(true)) return false;

        conn_thread_ = std::thread(&PharmacyBusinessApp::connect_loop, this);
        metrics_thread_ = std::thread(&PharmacyBusinessApp::metrics_loop, this);

        std::cout << "[PharmacyApp] Started\n";
        return true;
    }

    void stop() {
        if (!running_.exchange(false)) return;

        if (fd_ >= 0) {
#ifdef _WIN32
            closesocket(fd_);
#else
            close(fd_);
#endif
            fd_ = -1;
        }

        if (conn_thread_.joinable()) conn_thread_.join();
        if (metrics_thread_.joinable()) metrics_thread_.join();

        connected_.store(false);
        std::cout << "[PharmacyApp] Stopped\n";
    }

    // 模拟业务数据更新（实际项目中从硬件/数据库读取）
    void update_metrics(const PharmacyMetrics& m) {
        metrics_ = m;
    }

    void update_status(const PharmacyStatus& s) {
        status_ = s;
    }

private:
    void connect_loop() {
        while (running_) {
            if (connect_to_socket()) {
                std::cout << "[PharmacyApp] Connected to device-agent\n";
                connected_.store(true);
                // 连接建立，发送 ready 状态（可选）
            } else {
                connected_.store(false);
                std::this_thread::sleep_for(std::chrono::seconds(3));
            }

            // 保持重连循环
            while (running_ && connected_.load()) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }
    }

    bool connect_to_socket() {
#ifdef _WIN32
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) return false;

        fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) return false;

        // TCP localhost:9091 (device-agent 用 TCP 模式)
        sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(9091);
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

        if (connect(fd_, (sockaddr*)&addr, sizeof(addr)) < 0) {
            closesocket(fd_);
            fd_ = -1;
            return false;
        }
#else
        fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd_ < 0) return false;

        sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

        if (connect(fd_, (sockaddr*)&addr, sizeof(addr)) < 0) {
            close(fd_);
            fd_ = -1;
            return false;
        }
#endif
        return true;
    }

    void metrics_loop() {
        while (running_) {
            std::this_thread::sleep_for(std::chrono::seconds(10));

            if (!connected_.load()) continue;

            // 发送业务指标
            char buf[2048];
            snprintf(buf, sizeof(buf),
                "{"
                "\"type\":\"metrics\","
                "\"data\":{"
                    "\"transactions_today\":%d,"
                    "\"successful_transactions\":%d,"
                    "\"failed_transactions\":%d,"
                    "\"inventory_alert_count\":%d,"
                    "\"printer_online\":%s,"
                    "\"network_healthy\":%s"
                "}"
                "}\n",
                metrics_.transactions_today,
                metrics_.successful_transactions,
                metrics_.failed_transactions,
                metrics_.inventory_alert_count,
                metrics_.printer_online ? "true" : "false",
                metrics_.network_healthy ? "true" : "false"
            );
            send_msg(buf);

            // 发送业务状态
            snprintf(buf, sizeof(buf),
                "{"
                "\"type\":\"status\","
                "\"data\":{"
                    "\"printer_status\":\"%s\","
                    "\"medicine_slots_total\":%d,"
                    "\"medicine_slots_available\":%d,"
                    "\"screen_touch_working\":%s,"
                    "\"payment_status\":\"%s\""
                "}"
                "}\n",
                status_.printer_status.c_str(),
                status_.medicine_slots_total,
                status_.medicine_slots_available,
                status_.screen_touch_working ? "true" : "false",
                status_.payment_status.c_str()
            );
            send_msg(buf);
        }
    }

    void send_msg(const std::string& msg) {
        if (fd_ < 0) return;
        send(fd_, msg.c_str(), msg.size(), 0);
    }

    std::string socket_path_;
    int fd_ = -1;
    std::atomic<bool> connected_;
    std::atomic<bool> running_;

    PharmacyMetrics metrics_;
    PharmacyStatus status_;

    std::thread conn_thread_;
    std::thread metrics_thread_;
};

}  // namespace business
