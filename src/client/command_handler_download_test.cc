#include "client/command_handler.h"
#include "download/idownload_manager.h"
#include "executor/executor.h"

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

class FakeExecutor : public device_agent::Executor {
public:
    std::string reboot(bool, const std::string&, std::string&) override {
        return "pending";
    }
    void updateConfig(const std::string&, const std::string&, std::string&) override {}
    void upgradeFirmware(const std::string&, const std::string&, std::string&) override {}
    void upgradeApp(const std::string&, const std::string&, const std::string&, std::string&) override {}
    device_agent::InstallOutcome installPackage(const std::string& packagePath,
                                                const std::string& args,
                                                const std::string& successCodes,
                                                int softTimeoutSeconds,
                                                const std::string& command_id,
                                                std::string& err) override {
        install_calls++;
        last_package_path = packagePath;
        last_args = args;
        last_success_codes = successCodes;
        last_soft_timeout_seconds = softTimeoutSeconds;
        last_command_id = command_id;
        err = install_error;
        return install_outcome;
    }

    int install_calls = 0;
    device_agent::InstallOutcome install_outcome = device_agent::InstallOutcome::SUCCESS;
    std::string install_error;
    std::string last_package_path;
    std::string last_args;
    std::string last_success_codes;
    int last_soft_timeout_seconds = 0;
    std::string last_command_id;
};

terminal_agent::v1::Command make_download_ready_command(const std::string& torrent_url,
                                                        bool install = false) {
    terminal_agent::v1::Command cmd;
    cmd.set_command_id("cmd-1");
    cmd.set_command_type("download_ready");
    std::string payload =
        "{\"batch_id\":\"batch-1\","
        "\"file_id\":\"file-1\","
        "\"file_type\":\"bin\","
        "\"download_url\":\"http://127.0.0.1:18080/file.bin\","
        "\"torrent_url\":\"" + torrent_url + "\","
        "\"sha256\":\"abc\","
        "\"file_size\":1024";
    if (install) {
        payload += ",\"install_args\":\"/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP-\","
                   "\"install_success_codes\":\"0,3010\"";
    }
    payload += "}";
    cmd.set_payload_json(payload);
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

#ifdef _WIN32
    {
        std::vector<terminal_agent::v1::ReleaseStatusRequest> reports;
        auto fake = std::make_shared<FakeDownloadManager>();
        auto executor = std::make_shared<FakeExecutor>();
        device_agent::CommandHandler handler(
            [](const terminal_agent::v1::CommandResult&) { return true; });
        handler.set_download_manager(fake);
        handler.set_executor(executor);
        handler.set_download_directory("C:\\DeviceAgent\\downloads");
        handler.set_release_status_reporter(
            [&reports](const terminal_agent::v1::ReleaseStatusRequest& report) {
                reports.push_back(report);
                return true;
            });

        const auto result = handler.execute_sync(make_download_ready_command("", true), 0);

        ok &= expect(result.status() == "success", "install command should dispatch");
        ok &= expect(executor->install_calls == 1, "installPackage should be called once");
        ok &= expect(executor->last_package_path == "C:\\DeviceAgent\\downloads\\file-1",
                     "installPackage should receive resolved download path");
        ok &= expect(executor->last_args == "/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP-",
                     "install args should be forwarded");
        ok &= expect(executor->last_success_codes == "0,3010",
                     "success codes should be forwarded");
        ok &= expect(executor->last_soft_timeout_seconds == 120,
                     "soft probe timeout should default to 120s when payload omits it");
        ok &= expect(reports.size() == 5,
                     "install success path should report downloading/progress/downloaded/installing/installed");
        if (reports.size() >= 5) {
            ok &= expect(reports[2].status() == terminal_agent::v1::RELEASE_DEVICE_STATUS_DOWNLOADED,
                         "install path should first report downloaded");
            ok &= expect(reports[3].status() == terminal_agent::v1::RELEASE_DEVICE_STATUS_INSTALLING,
                         "install path should report installing");
            ok &= expect(reports[4].status() == terminal_agent::v1::RELEASE_DEVICE_STATUS_INSTALLED,
                         "install path should report installed");
        }
    }

    {
        std::vector<terminal_agent::v1::ReleaseStatusRequest> reports;
        auto fake = std::make_shared<FakeDownloadManager>();
        auto executor = std::make_shared<FakeExecutor>();
        executor->install_outcome = device_agent::InstallOutcome::FAILED;
        executor->install_error = "installer exit code 5";
        device_agent::CommandHandler handler(
            [](const terminal_agent::v1::CommandResult&) { return true; });
        handler.set_download_manager(fake);
        handler.set_executor(executor);
        handler.set_release_status_reporter(
            [&reports](const terminal_agent::v1::ReleaseStatusRequest& report) {
                reports.push_back(report);
                return true;
            });

        const auto result = handler.execute_sync(make_download_ready_command("", true), 0);

        ok &= expect(result.status() == "success", "install failure command should still dispatch");
        ok &= expect(executor->install_calls == 1, "failed install should call installPackage once");
        ok &= expect(reports.size() == 5,
                     "install failure path should report downloading/progress/downloaded/installing/install_failed");
        if (reports.size() >= 5) {
            ok &= expect(reports[4].status() == terminal_agent::v1::RELEASE_DEVICE_STATUS_INSTALL_FAILED,
                         "install failure path should report install_failed");
            ok &= expect(reports[4].error_code() == terminal_agent::v1::RELEASE_ERROR_CODE_INSTALL_ERROR,
                         "install failure path should set INSTALL_ERROR");
            ok &= expect(reports[4].error_message() == "installer exit code 5",
                         "install failure path should forward install error");
        }
    }

    {
        // NEEDS_ATTENDED:试静默软超时 → 复用 INSTALL_FAILED + BUSINESS_ERROR + sentinel。
        std::vector<terminal_agent::v1::ReleaseStatusRequest> reports;
        auto fake = std::make_shared<FakeDownloadManager>();
        auto executor = std::make_shared<FakeExecutor>();
        executor->install_outcome = device_agent::InstallOutcome::NEEDS_ATTENDED;
        executor->install_error = "silent install timed out after 120s, needs attended install";
        device_agent::CommandHandler handler(
            [](const terminal_agent::v1::CommandResult&) { return true; });
        handler.set_download_manager(fake);
        handler.set_executor(executor);
        handler.set_download_directory("C:\\DeviceAgent\\downloads");
        handler.set_release_status_reporter(
            [&reports](const terminal_agent::v1::ReleaseStatusRequest& report) {
                reports.push_back(report);
                return true;
            });

        const auto result = handler.execute_sync(make_download_ready_command("", true), 0);

        ok &= expect(result.status() == "success", "needs-attended command should still dispatch");
        ok &= expect(executor->install_calls == 1, "needs-attended should call installPackage once");
        ok &= expect(reports.size() == 5,
                     "needs-attended path should report downloading/progress/downloaded/installing/install_failed");
        if (reports.size() >= 5) {
            ok &= expect(reports[4].status() == terminal_agent::v1::RELEASE_DEVICE_STATUS_INSTALL_FAILED,
                         "needs-attended path should report install_failed status");
            ok &= expect(reports[4].error_code() == terminal_agent::v1::RELEASE_ERROR_CODE_BUSINESS_ERROR,
                         "needs-attended path should set BUSINESS_ERROR");
            ok &= expect(reports[4].error_message().rfind("NEEDS_ATTENDED:", 0) == 0,
                         "needs-attended path should carry NEEDS_ATTENDED: sentinel prefix");
            ok &= expect(reports[4].error_message().find("package=") != std::string::npos,
                         "needs-attended sentinel should include package path");
        }
    }
#endif

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
