#include <cstdlib>
#include <csignal>
#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>

#include "pharmacy_app.h"

namespace {

std::atomic<bool> g_running{true};

void signal_handler(int sig) {
    (void)sig;
    g_running.store(false);
}

}  // anonymous namespace

int main(int argc, char* argv[]) {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    std::string socket_path = "/var/run/device-agent/business.sock";
    if (argc > 1) {
        socket_path = argv[1];
    }

    std::cout << "=== Pharmacy Business App ===\n";
    std::cout << "Connecting to: " << socket_path << "\n";

    business::PharmacyBusinessApp app(socket_path);

    std::cerr << "[PharmacyApp] Starting..." << std::endl;

    if (!app.start()) {
        std::cerr << "Failed to start app\n";
        return 1;
    }

    // 模拟业务数据更新
    int tick = 0;
    while (g_running.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        tick++;

        business::PharmacyMetrics m;
        m.transactions_today = tick * 3;
        m.successful_transactions = tick * 3 - tick / 2;
        m.failed_transactions = tick / 2;
        m.inventory_alert_count = (tick % 10 == 0) ? 1 : 0;
        m.printer_online = (tick % 7 != 0);
        m.network_healthy = true;

        business::PharmacyStatus s;
        s.printer_status = (tick % 7 == 0) ? "error" : "ok";
        s.medicine_slots_available = 20 - (tick % 5);
        s.screen_touch_working = true;
        s.payment_status = "ok";

        app.update_metrics(m);
        app.update_status(s);

        std::cout << "[PharmacyApp] Tick " << tick
                  << ": " << m.transactions_today << " transactions today"
                  << ", slots: " << s.medicine_slots_available << "/" << s.medicine_slots_total
                  << "\n";
    }

    app.stop();
    std::cout << "=== Pharmacy App stopped ===\n";
    return 0;
}
