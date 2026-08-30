#include "observability/observability.h"

#include <algorithm>
#include <utility>

#include "logger/logger.h"
#include "version/build_info.h"

namespace device_agent {
namespace observability {
namespace {

constexpr auto kSnapshotMaxAge = std::chrono::seconds(30);

template <typename... Values>
Availability availability_for(const Values&... values) {
    const bool present[] = {values.has_value()...};
    const auto count = static_cast<std::size_t>(
        std::count(std::begin(present), std::end(present), true));
    if (count == 0) {
        return Availability::kUnavailable;
    }
    if (count == sizeof...(values)) {
        return Availability::kAvailable;
    }
    return Availability::kPartial;
}

template <typename T>
void set_if_present(const std::optional<T>& value,
                    const std::function<void(const T&)>& setter) {
    if (value.has_value()) {
        setter(*value);
    }
}

}  // namespace

ObservabilitySnapshot finalize_observation(RawObservation raw) {
    ObservabilitySnapshot out;
    out.inventory_availability = availability_for(
        raw.platform, raw.os_arch, raw.hostname, raw.os_name, raw.os_version,
        raw.cpu_model, raw.cpu_logical_count, raw.memory_total_bytes,
        raw.system_disk_total_bytes);
    out.telemetry_availability = availability_for(
        raw.cpu_percent, raw.memory_percent, raw.system_disk_percent,
        raw.network_rx_bytes, raw.network_tx_bytes, raw.uptime_seconds);
    out.network_availability = availability_for(
        raw.lan_ip, raw.lan_cidr, raw.net_type);
    out.values = std::move(raw);
    out.collected_at = std::chrono::system_clock::now();
    return out;
}

void populate_heartbeat(const ObservabilitySnapshot& snapshot,
                        terminal_agent::v1::HeartbeatRequest* out) {
    if (out == nullptr) {
        return;
    }
    const auto now = std::chrono::system_clock::now();
    if (snapshot.collected_at.time_since_epoch().count() == 0 ||
        snapshot.collected_at > now || now - snapshot.collected_at > kSnapshotMaxAge) {
        return;
    }
    if (snapshot.telemetry_availability != Availability::kAvailable) {
        return;
    }
    out->set_cpu_percent(*snapshot.values.cpu_percent);
    out->set_memory_percent(*snapshot.values.memory_percent);
    out->set_disk_percent(*snapshot.values.system_disk_percent);
    out->set_uptime_seconds(*snapshot.values.uptime_seconds);
}

void populate_status(const ObservabilitySnapshot& snapshot,
                     std::int64_t p2p_upload_bytes,
                     std::int64_t p2p_upload_bytes_cellular,
                     terminal_agent::v1::StatusReport* out) {
    if (out == nullptr) {
        return;
    }
    const auto now = std::chrono::system_clock::now();
    if (snapshot.collected_at.time_since_epoch().count() == 0 ||
        snapshot.collected_at > now || now - snapshot.collected_at > kSnapshotMaxAge) {
        return;
    }
    if (snapshot.inventory_availability != Availability::kUnavailable) {
        auto* inventory = out->mutable_inventory();
        inventory->set_agent_version(agent_version());
        set_if_present<std::string>(snapshot.values.platform,
            [inventory](const std::string& value) { inventory->set_platform(value); });
        set_if_present<std::string>(snapshot.values.os_arch,
            [inventory](const std::string& value) { inventory->set_os_arch(value); });
        set_if_present<std::string>(snapshot.values.hostname,
            [inventory](const std::string& value) { inventory->set_hostname(value); });
        set_if_present<std::string>(snapshot.values.os_name,
            [inventory](const std::string& value) { inventory->set_os_name(value); });
        set_if_present<std::string>(snapshot.values.os_version,
            [inventory](const std::string& value) { inventory->set_os_version(value); });
        set_if_present<std::string>(snapshot.values.cpu_model,
            [inventory](const std::string& value) { inventory->set_cpu_model(value); });
        set_if_present<std::int32_t>(snapshot.values.cpu_logical_count,
            [inventory](std::int32_t value) { inventory->set_cpu_logical_count(value); });
        set_if_present<std::int64_t>(snapshot.values.memory_total_bytes,
            [inventory](std::int64_t value) { inventory->set_memory_total_bytes(value); });
        set_if_present<std::int64_t>(snapshot.values.system_disk_total_bytes,
            [inventory](std::int64_t value) { inventory->set_system_disk_total_bytes(value); });
    }

    if (snapshot.telemetry_availability == Availability::kAvailable) {
        auto* metrics = out->mutable_metrics();
        metrics->set_cpu_percent(*snapshot.values.cpu_percent);
        metrics->set_memory_percent(*snapshot.values.memory_percent);
        metrics->set_disk_percent(*snapshot.values.system_disk_percent);
        metrics->set_network_rx_bytes(*snapshot.values.network_rx_bytes);
        metrics->set_network_tx_bytes(*snapshot.values.network_tx_bytes);
        metrics->set_uptime_seconds(*snapshot.values.uptime_seconds);
        metrics->set_p2p_upload_bytes(p2p_upload_bytes);
        metrics->set_p2p_upload_bytes_cellular(p2p_upload_bytes_cellular);
    }

    if (snapshot.network_availability != Availability::kUnavailable) {
        auto* network = out->mutable_network_info();
        set_if_present<std::string>(snapshot.values.lan_ip,
            [network](const std::string& value) { network->set_lan_ip(value); });
        set_if_present<std::string>(snapshot.values.lan_cidr,
            [network](const std::string& value) { network->set_lan_cidr(value); });
        set_if_present<terminal_agent::v1::NetworkType>(snapshot.values.net_type,
            [network](terminal_agent::v1::NetworkType value) { network->set_net_type(value); });
    }
}

ObservabilitySampler::ObservabilitySampler(Collector collector,
                                           std::chrono::milliseconds interval)
    : collector_(std::move(collector)), interval_(interval) {}

ObservabilitySampler::~ObservabilitySampler() {
    stop();
}

void ObservabilitySampler::start() {
    std::lock_guard<std::mutex> lock(wait_mu_);
    if (running_) {
        return;
    }
    running_ = true;
    worker_ = std::thread(&ObservabilitySampler::run, this);
}

void ObservabilitySampler::stop() {
    {
        std::lock_guard<std::mutex> lock(wait_mu_);
        if (!running_) {
            return;
        }
        running_ = false;
    }
    wait_cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}

std::optional<ObservabilitySnapshot> ObservabilitySampler::latest() const {
    std::lock_guard<std::mutex> lock(snapshot_mu_);
    return snapshot_;
}

void ObservabilitySampler::run() {
    while (true) {
        {
            std::lock_guard<std::mutex> lock(wait_mu_);
            if (!running_) {
                break;
            }
        }

        ObservabilitySnapshot next = collector_();
        for (const auto& error : next.values.errors) {
            LOG_WARN("Observability collection failed: stage=" + error.stage +
                     " error=" + error.message +
                     " code=" + std::to_string(error.native_code));
        }
        {
            std::lock_guard<std::mutex> lock(snapshot_mu_);
            snapshot_ = std::move(next);
        }

        std::unique_lock<std::mutex> lock(wait_mu_);
        if (wait_cv_.wait_for(lock, interval_, [this] { return !running_; })) {
            break;
        }
    }
}

}  // namespace observability
}  // namespace device_agent
