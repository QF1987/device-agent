#include <cstdlib>
#include <csignal>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <atomic>

#include "client/device_client.h"
#include "client/command_handler.h"
#include "config/config.h"
#include "logger/logger.h"
#include "bridge/bridge.h"

namespace {

std::atomic<bool> g_running{true};

void signal_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        g_running.store(false);
    }
}

void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " [OPTIONS]\n"
              << "Options:\n"
              << "  -c <file>    Config file (JSON), default: /etc/device-agent/config.json\n"
              << "  -e           Load config from environment variables\n"
              << "  -h           Show this help\n";
}

}  // anonymous namespace

int main(int argc, char* argv[]) {
    // Parse args
    std::string config_file;
    bool use_env = false;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-c" && i + 1 < argc) {
            config_file = argv[++i];
        } else if (arg == "-e") {
            use_env = true;
        } else if (arg == "-h") {
            print_usage(argv[0]);
            return 0;
        }
    }

    // Signal handler
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Load config
    device_agent::Config config;
    if (use_env) {
        config = device_agent::Config::load_from_env();
    } else {
        if (config_file.empty()) {
            config_file = "/etc/device-agent/config.json";
        }
        config = device_agent::Config::load(config_file);
    }

    // Setup logging
    if (config.log_level == "debug") {
        device_agent::Logger::instance().set_level(device_agent::LogLevel::DEBUG);
    } else if (config.log_level == "warn") {
        device_agent::Logger::instance().set_level(device_agent::LogLevel::WARN);
    } else if (config.log_level == "error") {
        device_agent::Logger::instance().set_level(device_agent::LogLevel::ERROR);
    } else {
        device_agent::Logger::instance().set_level(device_agent::LogLevel::INFO);
    }

    if (!config.log_file.empty()) {
        device_agent::Logger::instance().set_output(config.log_file);
    }

    LOG_INFO("=== device-agent starting ===");
    LOG_INFO("Device ID: " + config.auth.device_id);
    LOG_INFO("Server: " + config.server.server_address());

    if (config.auth.device_id.empty() || config.auth.token.empty()) {
        LOG_ERROR("device_id and token are required");
        return 1;
    }

    // Create business bridge
    std::shared_ptr<device_agent::IBusinessBridge> bridge;
    if (config.business_bridge.type == "socket") {
        bridge = std::make_shared<device_agent::SocketBridge>(config.business_bridge.path);
        LOG_INFO("Business bridge: socket (" + config.business_bridge.path + ")");
    } else {
        bridge = std::make_shared<device_agent::NullBridge>();
        LOG_INFO("Business bridge: null (no business data)");
    }

    // Command result reporter
    std::shared_ptr<device_agent::DeviceClient> client(
        new device_agent::DeviceClient(config));

    device_agent::CommandHandler handler(
        [client](const terminal_agent::v1::CommandResult& result) -> bool {
            return client->report_command_result(result);
        });

    // Set command callback
    client->set_command_callback(
        [&handler](const terminal_agent::v1::Command& cmd) {
            handler.handle(cmd);
        });

    // Start bridge and register handler
    bridge->set_handler(nullptr);  // No business handler yet
    bridge->start();

    // Start
    client->start();

    // Metrics polling thread (injects business data into heartbeats)
    std::thread metrics_thread([&]() {
        while (g_running.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(10));
            // TODO: poll business metrics and inject into status report
            (void)bridge;
        }
    });

    // Wait for signal
    while (g_running.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    LOG_INFO("Shutting down...");
    client->stop();
    bridge->stop();
    metrics_thread.join();

    LOG_INFO("=== device-agent stopped ===");
    return 0;
}
