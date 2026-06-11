#include "client/command_handler.h"
#include "download/idownload_manager.h"

#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

class FakeDownloadManager : public device_agent::IDownloadManager {
public:
    void download(const device_agent::DownloadRequest& req,
                  ProgressCallback on_progress,
                  CompleteCallback on_complete) override {
        requests.push_back(req);
        if (emit_progress && on_progress) {
            on_progress(device_agent::DownloadProgress{progress_bytes, req.file_size, 50});
        }
        if (on_complete) {
            on_complete(complete_success, complete_error, completion_telemetry);
        }
    }

    void cancel() override {}
    bool is_downloading() const override { return false; }

    bool emit_progress = true;
    int64_t progress_bytes = 512;
    bool complete_success = true;
    std::string complete_error;
    device_agent::DownloadCompletionTelemetry completion_telemetry;
    std::vector<device_agent::DownloadRequest> requests;
};

terminal_agent::v1::Command make_download_ready_command(const std::string& torrent_url) {
    terminal_agent::v1::Command cmd;
    cmd.set_command_id("cmd-1");
    cmd.set_command_type("download_ready");
    cmd.set_payload_json(
        "{\"batch_id\":\"batch-1\","
        "\"file_id\":\"file-1\","
        "\"file_type\":\"bin\","
        "\"download_url\":\"http://127.0.0.1:18080/file.bin\","
        "\"torrent_url\":\"" + torrent_url + "\","
        "\"sha256\":\"abc\","
        "\"file_size\":1024}");
    return cmd;
}

}  // namespace

bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        return false;
    }
    return true;
}

int main() {
    bool ok = true;
    {
        std::vector<terminal_agent::v1::ReleaseStatusRequest> reports;
        auto fake = std::make_shared<FakeDownloadManager>();
        fake->completion_telemetry.completion_path =
            terminal_agent::v1::P2P_PRIMARY;
        fake->completion_telemetry.peer_bytes = 321;
        fake->completion_telemetry.web_seed_bytes = 191;
        device_agent::CommandHandler handler(
            [](const terminal_agent::v1::CommandResult&) { return true; });
        handler.set_download_manager(fake);
        handler.set_download_directory("/tmp/device-agent-test-downloads");
        handler.set_release_status_reporter(
            [&reports](const terminal_agent::v1::ReleaseStatusRequest& report) {
                reports.push_back(report);
                return true;
            });

        const auto result = handler.execute_sync(
            make_download_ready_command("http://127.0.0.1:18080/file.torrent"), 0);

        ok &= expect(result.status() == "success", "download_ready command result should succeed");
        ok &= expect(fake->requests.size() == 1, "download manager should receive one request");
        ok &= expect(fake->requests[0].dest_path == "/tmp/device-agent-test-downloads",
                     "download directory should be passed into request");
        ok &= expect(reports.size() == 3, "success path should report downloading/progress/downloaded");
        if (reports.size() >= 3) {
            ok &= expect(reports[0].status() == terminal_agent::v1::RELEASE_DEVICE_STATUS_DOWNLOADING,
                         "initial status should be downloading");
            ok &= expect(reports[0].downloaded_bytes() == 0,
                         "initial downloading bytes should be zero");
            ok &= expect(reports[1].status() == terminal_agent::v1::RELEASE_DEVICE_STATUS_DOWNLOADING,
                         "progress status should be downloading");
            ok &= expect(reports[1].downloaded_bytes() == 512,
                         "progress bytes should be forwarded");
            ok &= expect(reports[2].status() == terminal_agent::v1::RELEASE_DEVICE_STATUS_DOWNLOADED,
                         "completion status should be downloaded");
            ok &= expect(reports[2].downloaded_bytes() == 512,
                         "completion bytes should use last progress");
            ok &= expect(reports[2].completion_path() ==
                             terminal_agent::v1::P2P_PRIMARY,
                         "completion path should be forwarded from download telemetry");
            ok &= expect(reports[2].peer_bytes() == 321,
                         "peer bytes should be forwarded from download telemetry");
            ok &= expect(reports[2].web_seed_bytes() == 191,
                         "web seed bytes should be forwarded from download telemetry");
        }
    }

    {
        std::vector<terminal_agent::v1::ReleaseStatusRequest> reports;
        auto fake = std::make_shared<FakeDownloadManager>();
        fake->emit_progress = false;
        fake->complete_success = false;
        fake->complete_error = "network down";
        device_agent::CommandHandler handler(
            [](const terminal_agent::v1::CommandResult&) { return true; });
        handler.set_download_manager(fake);
        handler.set_release_status_reporter(
            [&reports](const terminal_agent::v1::ReleaseStatusRequest& report) {
                reports.push_back(report);
                return true;
            });

        const auto result = handler.execute_sync(make_download_ready_command(""), 0);

        ok &= expect(result.status() == "success", "HTTP fallback command result should dispatch");
        ok &= expect(fake->requests.size() == 1, "HTTP fallback should call download manager");
        ok &= expect(fake->requests[0].torrent_url.empty(), "HTTP fallback should preserve empty torrent_url");
        ok &= expect(fake->requests[0].url == "http://127.0.0.1:18080/file.bin",
                     "HTTP fallback should preserve download_url");
        ok &= expect(reports.size() == 2, "failure path should report downloading/download_failed");
        if (reports.size() >= 2) {
            ok &= expect(reports[0].status() == terminal_agent::v1::RELEASE_DEVICE_STATUS_DOWNLOADING,
                         "failure path initial status should be downloading");
            ok &= expect(reports[1].status() == terminal_agent::v1::RELEASE_DEVICE_STATUS_DOWNLOAD_FAILED,
                         "failure path completion status should be download_failed");
            ok &= expect(reports[1].error_code() == terminal_agent::v1::RELEASE_ERROR_CODE_NETWORK_ERROR,
                         "failure path should set NETWORK_ERROR");
            ok &= expect(reports[1].error_message() == "network down",
                         "failure path should forward error message");
        }
    }

    return ok ? 0 : 1;
}
