#include "client/device_client.h"
#include "capability/capability_manifest.h"
#include "download/p2p_upload_counters.h"
#include "client/network_info.h"
#include "logger/logger.h"
#ifdef _WIN32
#include "observability/windows_observability.h"
#endif
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

#ifdef _WIN32
    observability_sampler_.reset(new observability::ObservabilitySampler(
        observability::collect_windows_observability));
#endif

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

#ifdef _WIN32
    observability_sampler_->start();
#endif

    heartbeat_thread_ = std::thread(&DeviceClient::heartbeat_loop, this);
    status_report_thread_ = std::thread(&DeviceClient::status_report_loop, this);
    command_stream_thread_ = std::thread(&DeviceClient::command_stream_loop, this);
}

void DeviceClient::interruptible_sleep(std::chrono::seconds duration) {
    std::unique_lock<std::mutex> lk(stop_mu_);
    // running_ 变 false 时立即返回;否则最多等 duration
    stop_cv_.wait_for(lk, duration, [this] { return !running_.load(); });
}

void DeviceClient::stop() {
    if (!running_.exchange(false)) {
        return;
    }

    LOG_INFO("DeviceClient stopping...");

    // RV-20260602-20: 唤醒 sleep 在心跳/状态/重连退避里的线程(否则 join 要等满 30s/300s)。
    stop_cv_.notify_all();
    // RV-20260602-20: 取消阻塞在 reader->Read() 的 CommandStream,否则 command_stream_thread_ join 卡死。
    {
        std::lock_guard<std::mutex> l(cmd_ctx_mu_);
        if (cmd_ctx_) cmd_ctx_->TryCancel();
    }

    if (heartbeat_thread_.joinable()) heartbeat_thread_.join();
    if (status_report_thread_.joinable()) status_report_thread_.join();
    if (command_stream_thread_.joinable()) command_stream_thread_.join();

#ifdef _WIN32
    observability_sampler_->stop();
#endif

    connected_.store(false);
    LOG_INFO("DeviceClient stopped");
}

void DeviceClient::set_command_callback(CommandCallback cb) {
    command_callback_ = std::move(cb);
}

void DeviceClient::set_runtime_capability_provider(RuntimeCapabilityProvider provider) {
    runtime_capability_provider_ = std::move(provider);
}

void DeviceClient::heartbeat_loop() {
    int retry_count = 0;

    while (running_) {
        terminal_agent::v1::HeartbeatRequest req;
        req.set_device_id(config_.auth.device_id);
        req.set_timestamp(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());

        capability::RuntimeCapabilities runtime_capabilities;
        if (runtime_capability_provider_) {
            runtime_capabilities = runtime_capability_provider_();
        }
        capability::populate_current_manifest(runtime_capabilities, req.mutable_capability());

#ifdef _WIN32
        const auto snapshot = observability_sampler_->latest();
        if (snapshot.has_value()) {
            observability::populate_heartbeat(*snapshot, &req);
        }
#else
        // 非 Windows collector 不在 S2 范围，保留既有兼容行为。
        req.set_cpu_percent(0);
        req.set_memory_percent(0);
        req.set_disk_percent(0);
        req.set_uptime_seconds(0);
#endif

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

        interruptible_sleep(std::chrono::seconds(config_.heartbeat.interval_seconds));
    }
}

void DeviceClient::status_report_loop() {
    while (running_) {
        interruptible_sleep(std::chrono::seconds(config_.status_report.interval_seconds));

        if (!connected_.load()) continue;

        terminal_agent::v1::StatusReport status;
        status.set_device_id(config_.auth.device_id);
        status.set_timestamp(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        status.set_status("online");
#ifdef _WIN32
        const auto snapshot = observability_sampler_->latest();
        if (snapshot.has_value()) {
            const auto p2p_upload = p2p_upload_counters();
            observability::populate_status(
                *snapshot, p2p_upload.total, p2p_upload.cellular, &status);
        }
#else
        status.set_firmware_version("v1.0.0");
        auto* metrics = status.mutable_metrics();
        metrics->set_cpu_percent(0);
        metrics->set_memory_percent(0);
        metrics->set_disk_percent(0);
        metrics->set_network_rx_bytes(0);
        metrics->set_network_tx_bytes(0);
        metrics->set_uptime_seconds(0);
        // 蜂窝守门:P2P 做种上传分桶计数(ADR-20260612-01;老 server 忽略未知字段)
        const auto p2p_upload = p2p_upload_counters();
        metrics->set_p2p_upload_bytes(p2p_upload.total);
        metrics->set_p2p_upload_bytes_cellular(p2p_upload.cellular);
        populate_network_info_proto(status.mutable_network_info());
#endif

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
            interruptible_sleep(std::chrono::seconds(1));
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

    // RV-20260602-20: 暴露本 ctx 给 stop() 以便 TryCancel;RAII 在 ctx 析构前置空(声明在 ctx 之后
    // → 先于 ctx 析构),保证 cancel-vs-destroy 不发生 UAF。
    {
        std::lock_guard<std::mutex> l(cmd_ctx_mu_);
        cmd_ctx_ = &ctx;
    }
    struct CmdCtxGuard {
        DeviceClient* self;
        ~CmdCtxGuard() {
            std::lock_guard<std::mutex> l(self->cmd_ctx_mu_);
            self->cmd_ctx_ = nullptr;
        }
    } cmd_ctx_guard{this};

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
    // 注意：不能用 stub_mu_，因为 CommandStream 持有 stub_mu_ 的同时会调用 command_callback_
    // 而 command_callback_ 最终会调用本方法，导致死锁。
    // gRPC stub 是线程安全的（不同 RPC 调用之间），不需要额外加锁。

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

bool DeviceClient::report_release_status(
        const terminal_agent::v1::ReleaseStatusRequest& status) {
    terminal_agent::v1::ReleaseStatusRequest request(status);
    if (request.device_id().empty()) {
        request.set_device_id(config_.auth.device_id);
    }
    if (request.device_id().empty()) {
        LOG_ERROR("ReportReleaseStatus skipped: device_id is empty");
        return false;
    }

    terminal_agent::v1::ReleaseStatusResponse resp;
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(10));
    set_auth_metadata(ctx);

    grpc::Status st = device_stub_->ReportReleaseStatus(&ctx, request, &resp);
    if (!st.ok()) {
        LOG_ERROR("ReportReleaseStatus failed: " + st.error_message());
        return false;
    }
    return resp.accepted();
}

void DeviceClient::set_auth_metadata(grpc::ClientContext& ctx) {
    ctx.AddMetadata("x-device-id", config_.auth.device_id);
    ctx.AddMetadata("x-device-token", config_.auth.token);
}

}  // namespace device_agent
