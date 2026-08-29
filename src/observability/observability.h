#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "terminal_agent/v1/device.pb.h"

namespace device_agent {
namespace observability {

enum class Availability {
    kAvailable,
    kPartial,
    kUnavailable,
};

struct CollectionError {
    std::string stage;
    std::string message;
    std::uint32_t native_code = 0;
};

struct RawObservation {
    std::optional<std::string> platform;
    std::optional<std::string> os_arch;
    std::optional<std::string> hostname;
    std::optional<std::string> os_name;
    std::optional<std::string> os_version;
    std::optional<std::string> cpu_model;
    std::optional<std::int32_t> cpu_logical_count;
    std::optional<std::int64_t> memory_total_bytes;
    std::optional<std::int64_t> system_disk_total_bytes;

    std::optional<float> cpu_percent;
    std::optional<float> memory_percent;
    std::optional<float> system_disk_percent;
    std::optional<std::int64_t> network_rx_bytes;
    std::optional<std::int64_t> network_tx_bytes;
    std::optional<std::int32_t> uptime_seconds;

    std::optional<std::string> lan_ip;
    std::optional<std::string> lan_cidr;
    std::optional<terminal_agent::v1::NetworkType> net_type;
    std::vector<CollectionError> errors;
};

struct ObservabilitySnapshot {
    Availability inventory_availability = Availability::kUnavailable;
    Availability telemetry_availability = Availability::kUnavailable;
    Availability network_availability = Availability::kUnavailable;
    RawObservation values;
    std::chrono::system_clock::time_point collected_at;
};

using Collector = std::function<ObservabilitySnapshot()>;

ObservabilitySnapshot finalize_observation(RawObservation raw);
void populate_windows_capability(terminal_agent::v1::DeviceCapability* out);
void populate_heartbeat(const ObservabilitySnapshot& snapshot,
                        terminal_agent::v1::HeartbeatRequest* out);
void populate_status(const ObservabilitySnapshot& snapshot,
                     terminal_agent::v1::StatusReport* out);

class ObservabilitySampler {
public:
    explicit ObservabilitySampler(
        Collector collector,
        std::chrono::milliseconds interval = std::chrono::seconds(10));
    ~ObservabilitySampler();

    void start();
    void stop();
    std::optional<ObservabilitySnapshot> latest() const;

private:
    void run();

    Collector collector_;
    std::chrono::milliseconds interval_;
    mutable std::mutex snapshot_mu_;
    std::optional<ObservabilitySnapshot> snapshot_;
    std::mutex wait_mu_;
    std::condition_variable wait_cv_;
    std::thread worker_;
    bool running_ = false;
};

}  // namespace observability
}  // namespace device_agent
