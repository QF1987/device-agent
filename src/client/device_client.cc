#include "client/device_client.h"
#include "logger/logger.h"
#include <chrono>
#include <cmath>

namespace device_agent {

DeviceClient::DeviceClient(const Config& config)
    : config_(config) {

    // Build channel
    grpc::ChannelArguments args;
    args.SetMaxReceiveMessageSize(128 * 1024 * 1024);  // 128MB
    args.SetInt(GRPC_ARG_KEEPALIVE_TIME_MS, 30000);    // 30s keepalive
    args.SetInt(GRPC_ARG_KEEPALIVE_TIMEOUT_MS, 10000); // 10s timeout

    auto channel = grpc::CreateCustomChannel(
        config_.server.server_address(),
        grpc::InsecureChannelCredentials(),
        args);

    device_stub_ = terminal_agent::v1::DeviceService::NewStub(channel);
    command_stub_ = terminal_agent::v1::CommandService::NewStub(channel);

    LOG_INFO("DeviceClient created, server: " + config_.server.server_address());
}

DeviceClient::~DeviceClient() {
    stop();
}

void DeviceClient::start() {
    if (running_.exchange(true)) {
        LOG_WARN("DeviceClient already started");
        return;
    }

    LOG_INFO("DeviceClient starting...");

    heartbeat_thread_ = std::thread(&DeviceClient::heartbeat_loop, this);
    status_report_thread_ = std::thread(&DeviceClient::status_report_loop, this);
    command_stream_thread_ = std::thread(&DeviceClient::command_stream_loop, this);
}

void DeviceClient::stop() {
    if (!running_.exchange(false)) {
        return;
    }

    LOG_INFO("DeviceClient stopping...");

    if (heartbeat_thread_.joinable()) heartbeat_thread_.join();
    if (status_report_thread_.joinable()) status_report_thread_.join();
    if (command_stream_thread_.joinable()) command_stream_thread_.join();

    connected_.store(false);
    LOG_INFO("DeviceClient stopped");
}

void DeviceClient::set_command_callback(CommandCallback cb) {
    command_callback_ = std::move(cb);
}

void DeviceClient::heartbeat_loop() {
    int retry_count = 0;

    while (running_) {
        terminal_agent::v1::HeartbeatRequest req;
        req.set_device_id(config_.auth.device_id);
        req.set_timestamp(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());

        // TODO: populate cpu/memory/disk metrics
        req.set_cpu_percent(0);
        req.set_memory_percent(0);
        req.set_disk_percent(0);
        req.set_uptime_seconds(0);

        terminal_agent::v1::HeartbeatResponse resp;

        grpc::ClientContext ctx;
        ctx.set_wait_for_ready(false);
        ctx.set_deadline(std::chrono::system_clock::now() +
            std::chrono::seconds(config_.heartbeat.timeout_seconds));

        set_auth_metadata(ctx);

        grpc::Status status = device_stub_->Heartbeat(&ctx, req, &resp);

        if (status.ok()) {
            connected_.store(true);
            retry_count = 0;
            if (resp.has_pending_command()) {
                LOG_DEBUG("Server indicates pending command");
            }
        } else {
            retry_count++;
            connected_.store(false);
            LOG_WARN("Heartbeat failed: " + status.error_message() +
                     " (retry " + std::to_string(retry_count) + ")");

            if (retry_count >= config_.heartbeat.max_retries) {
                LOG_ERROR("Heartbeat max retries exceeded, will attempt reconnect");
                reconnect_command_stream();
                retry_count = 0;
            }
        }

        std::this_thread::sleep_for(
            std::chrono::seconds(config_.heartbeat.interval_seconds));
    }
}

void DeviceClient::status_report_loop() {
    while (running_) {
        std::this_thread::sleep_for(
            std::chrono::seconds(config_.status_report.interval_seconds));

        if (!connected_.load()) continue;

        terminal_agent::v1::StatusReport status;
        status.set_device_id(config_.auth.device_id);
        status.set_timestamp(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        status.set_status("online");
        status.set_firmware_version("v1.0.0");

        auto* metrics = status.mutable_metrics();
        metrics->set_cpu_percent(0);
        metrics->set_memory_percent(0);
        metrics->set_disk_percent(0);
        metrics->set_network_rx_bytes(0);
        metrics->set_network_tx_bytes(0);
        metrics->set_uptime_seconds(0);

        terminal_agent::v1::StatusReportResponse resp;
        grpc::ClientContext ctx;
        ctx.set_deadline(std::chrono::system_clock::now() +
            std::chrono::seconds(config_.status_report.timeout_seconds));

        set_auth_metadata(ctx);

        grpc::Status st = device_stub_->ReportStatus(&ctx, status, &resp);
        if (!st.ok()) {
            LOG_WARN("StatusReport failed: " + st.error_message());
        }
    }
}

void DeviceClient::command_stream_loop() {
    while (running_) {
        reconnect_command_stream();

        // Wait before reconnecting (exponential backoff handled in reconnect)
        if (running_) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
}

void DeviceClient::reconnect_command_stream() {
    if (!running_) return;

    std::lock_guard<std::mutex> lock(stub_mu_);

    terminal_agent::v1::CommandStreamRequest req;
    req.set_device_id(config_.auth.device_id);

    grpc::ClientContext ctx;
    ctx.set_wait_for_ready(false);

    // gRPC keepalive for long-lived stream
    ctx.AddMetadata("x-keepalive", "true");

    std::unique_ptr<grpc::ClientReader<terminal_agent::v1::Command>> reader(
        command_stub_->CommandStream(&ctx, req));

    command_stream_running_.store(true);
    LOG_INFO("CommandStream connected");

    terminal_agent::v1::Command cmd;
    while (reader->Read(&cmd)) {
        command_stream_running_.store(true);
        if (command_callback_) {
            command_callback_(cmd);
        }
    }

    command_stream_running_.store(false);
    reader->Finish();
    LOG_WARN("CommandStream disconnected");
}

bool DeviceClient::report_status(const terminal_agent::v1::StatusReport& status) {
    std::lock_guard<std::mutex> lock(stub_mu_);

    terminal_agent::v1::StatusReportResponse resp;
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() +
        std::chrono::seconds(config_.status_report.timeout_seconds));

    set_auth_metadata(ctx);

    grpc::Status st = device_stub_->ReportStatus(&ctx, status, &resp);
    if (!st.ok()) {
        LOG_ERROR("ReportStatus failed: " + st.error_message());
        return false;
    }
    return resp.accepted();
}

bool DeviceClient::report_event(const terminal_agent::v1::EventReport& event) {
    std::lock_guard<std::mutex> lock(stub_mu_);

    terminal_agent::v1::EventReportResponse resp;
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(10));

    set_auth_metadata(ctx);

    grpc::Status st = device_stub_->ReportEvent(&ctx, event, &resp);
    if (!st.ok()) {
        LOG_ERROR("ReportEvent failed: " + st.error_message());
        return false;
    }
    return resp.accepted();
}

bool DeviceClient::report_command_result(
        const terminal_agent::v1::CommandResult& result) {
    std::lock_guard<std::mutex> lock(stub_mu_);

    terminal_agent::v1::CommandResultResponse resp;
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(10));

    set_auth_metadata(ctx);

    grpc::Status st = device_stub_->ReportCommandResult(&ctx, result, &resp);
    if (!st.ok()) {
        LOG_ERROR("ReportCommandResult failed: " + st.error_message());
        return false;
    }
    return resp.accepted();
}

void DeviceClient::set_auth_metadata(grpc::ClientContext& ctx) {
    ctx.AddMetadata("x-device-id", config_.auth.device_id);
    ctx.AddMetadata("x-device-token", config_.auth.token);
}

}  // namespace device_agent
