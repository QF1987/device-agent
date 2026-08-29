#include "observability/observability.h"

#include <cassert>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "enrollment/enrollment_manager.h"
#include "version/build_info.h"

namespace {

device_agent::observability::RawObservation complete_raw(float cpu_percent) {
    device_agent::observability::RawObservation raw;
    raw.platform = "win";
    raw.os_arch = "amd64";
    raw.hostname = "WIN10-TEST";
    raw.os_name = "Windows 10 Pro";
    raw.os_version = "10.0.19045";
    raw.cpu_model = "Test CPU";
    raw.cpu_logical_count = 8;
    raw.memory_total_bytes = 16LL * 1024 * 1024 * 1024;
    raw.system_disk_total_bytes = 512LL * 1024 * 1024 * 1024;
    raw.cpu_percent = cpu_percent;
    raw.memory_percent = 0.0f;
    raw.system_disk_percent = 25.0f;
    raw.network_rx_bytes = 100;
    raw.network_tx_bytes = 200;
    raw.uptime_seconds = 300;
    raw.lan_ip = "192.168.31.20";
    raw.lan_cidr = "192.168.31.0/24";
    raw.net_type = terminal_agent::v1::ETHERNET;
    return raw;
}

}  // namespace

int main() {
    using namespace device_agent::observability;

    const auto success = finalize_observation(complete_raw(12.5f));
    assert(success.inventory_availability == Availability::kAvailable);
    assert(success.telemetry_availability == Availability::kAvailable);
    assert(success.network_availability == Availability::kAvailable);

    terminal_agent::v1::HeartbeatRequest heartbeat;
    populate_heartbeat(success, &heartbeat);
    assert(heartbeat.cpu_percent() == 12.5f);
    assert(heartbeat.capability().supported_features_size() == 5);
    for (const auto& feature : heartbeat.capability().supported_features()) {
        assert(feature != "command_firmware");
        assert(feature != "p2p_config");
    }

    terminal_agent::v1::StatusReport status;
    populate_status(success, &status);
    assert(status.has_inventory());
    assert(status.has_metrics());
    assert(status.has_network_info());
    assert(status.inventory().agent_version() == device_agent::agent_version());
    assert(status.inventory().agent_version().find('v') != 0);

    auto zero_raw = complete_raw(0.0f);
    zero_raw.memory_percent = 0.0f;
    zero_raw.system_disk_percent = 0.0f;
    const auto zero = finalize_observation(std::move(zero_raw));
    terminal_agent::v1::StatusReport zero_status;
    populate_status(zero, &zero_status);
    assert(zero_status.has_metrics());
    assert(zero_status.metrics().cpu_percent() == 0.0f);
    assert(zero_status.metrics().memory_percent() == 0.0f);
    assert(zero_status.metrics().disk_percent() == 0.0f);

    auto permission_raw = complete_raw(1.0f);
    permission_raw.cpu_model.reset();
    permission_raw.memory_percent.reset();
    permission_raw.errors.push_back({"memory", "access denied", 5});
    const auto permission_failure = finalize_observation(std::move(permission_raw));
    assert(permission_failure.inventory_availability == Availability::kPartial);
    assert(permission_failure.telemetry_availability == Availability::kPartial);
    terminal_agent::v1::StatusReport permission_status;
    populate_status(permission_failure, &permission_status);
    assert(permission_status.has_inventory());
    assert(!permission_status.has_metrics());

    auto missing_disk_raw = complete_raw(1.0f);
    missing_disk_raw.system_disk_total_bytes.reset();
    missing_disk_raw.system_disk_percent.reset();
    const auto missing_disk = finalize_observation(std::move(missing_disk_raw));
    assert(missing_disk.inventory_availability == Availability::kPartial);
    assert(missing_disk.telemetry_availability == Availability::kPartial);

    auto missing_network_raw = complete_raw(1.0f);
    missing_network_raw.lan_ip.reset();
    missing_network_raw.lan_cidr.reset();
    missing_network_raw.net_type.reset();
    missing_network_raw.network_rx_bytes.reset();
    missing_network_raw.network_tx_bytes.reset();
    const auto missing_network = finalize_observation(std::move(missing_network_raw));
    assert(missing_network.network_availability == Availability::kUnavailable);
    assert(missing_network.telemetry_availability == Availability::kPartial);
    terminal_agent::v1::StatusReport missing_network_status;
    populate_status(missing_network, &missing_network_status);
    assert(!missing_network_status.has_network_info());
    assert(!missing_network_status.has_metrics());

    auto stale = success;
    stale.collected_at = std::chrono::system_clock::now() - std::chrono::minutes(1);
    terminal_agent::v1::HeartbeatRequest stale_heartbeat;
    populate_heartbeat(stale, &stale_heartbeat);
    assert(stale_heartbeat.has_capability());
    assert(stale_heartbeat.cpu_percent() == 0.0f);
    terminal_agent::v1::StatusReport stale_status;
    populate_status(stale, &stale_status);
    assert(!stale_status.has_inventory());
    assert(!stale_status.has_metrics());
    assert(!stale_status.has_network_info());

    terminal_agent::v1::EnrollRequest enroll;
    device_agent::enrollment::populate_enrollment_identity(&enroll);
    assert(enroll.agent_version() == device_agent::agent_version());

    std::mutex gate_mu;
    std::condition_variable gate_cv;
    bool collector_entered = false;
    bool release_collector = false;
    ObservabilitySampler sampler([&] {
        {
            std::unique_lock<std::mutex> lock(gate_mu);
            collector_entered = true;
            gate_cv.notify_all();
            gate_cv.wait(lock, [&] { return release_collector; });
        }
        return finalize_observation(complete_raw(2.0f));
    }, std::chrono::hours(1));
    sampler.start();
    {
        std::unique_lock<std::mutex> lock(gate_mu);
        gate_cv.wait(lock, [&] { return collector_entered; });
    }
    const auto before = std::chrono::steady_clock::now();
    const auto while_blocked = sampler.latest();
    const auto elapsed = std::chrono::steady_clock::now() - before;
    assert(!while_blocked.has_value());
    assert(elapsed < std::chrono::milliseconds(50));
    {
        std::lock_guard<std::mutex> lock(gate_mu);
        release_collector = true;
    }
    gate_cv.notify_all();
    sampler.stop();

    return 0;
}
