#include "remotedesktop/remote_desktop_runtime.h"

#include "logger/logger.h"
#include "remotedesktop/rfb/rfb_server.h"
#include "remotedesktop/tunnel/tunnel_client.h"

#ifdef _WIN32
#include "remotedesktop/platform/windows/windows_badge.h"
#include "remotedesktop/platform/windows/windows_input_injector.h"
#include "remotedesktop/platform/windows/windows_screen_capturer.h"
#include "remotedesktop/platform/windows/windows_session_launcher.h"

#include <winsock2.h>
#endif

#include <thread>
#include <utility>

namespace device_agent::remotedesktop {

namespace {

#ifdef _WIN32
struct WinsockGuard {
    bool ok = false;
    WinsockGuard() {
        WSADATA data{};
        ok = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }
    ~WinsockGuard() {
        if (ok) {
            WSACleanup();
        }
    }
};
#endif

}  // namespace

bool runRemoteDesktopChild(const RemoteDesktopRuntimeConfig& config,
                           std::atomic<bool>& stop,
                           std::string& err) {
    if (!config.enabled()) {
        err = "remote desktop config is incomplete";
        return false;
    }
    std::string host;
    std::string port;
    if (!tunnel::splitHostPort(config.relay_endpoint, host, port, err)) {
        return false;
    }

#ifdef _WIN32
    WinsockGuard winsock;
    if (!winsock.ok) {
        err = "WSAStartup failed";
        return false;
    }

    windows::WindowsScreenCapturer capturer;
    windows::WindowsInputInjector injector;
    windows::WindowsRemoteControlBadge badge;

    tunnel::TunnelClientConfig tunnel_cfg;
    tunnel_cfg.relay_host = host;
    tunnel_cfg.relay_port = port;
    tunnel_cfg.device_id = config.device_id;
    tunnel_cfg.token = config.token;
    tunnel_cfg.server_name = config.server_name;
    tunnel_cfg.insecure_tls = config.insecure_tls;
    tunnel_cfg.heartbeat_seconds = config.heartbeat_seconds;

    ScreenFrame probe;
    std::string probe_err;
    if (capturer.capture(probe, probe_err)) {
        tunnel_cfg.screen_w = probe.width;
        tunnel_cfg.screen_h = probe.height;
        LOG_INFO("RemoteDesktopRuntime: Windows capture " + std::to_string(probe.width) + "x" +
                 std::to_string(probe.height) + " mode=" + capturer.lastCaptureMode() +
                 " force_gdi=" + (capturer.forceGdi() ? "true" : "false") +
                 " gdi_max_fps=" + std::to_string(capturer.gdiMaxFps()) +
                 " gdi_tile_size=" + std::to_string(capturer.gdiTileSize()) +
                 " initial_dirty_rects=" + std::to_string(probe.dirty_rects.size()));
    } else {
        tunnel_cfg.screen_w = config.fallback_width;
        tunnel_cfg.screen_h = config.fallback_height;
        LOG_WARN("RemoteDesktopRuntime: initial capture failed, using fallback size: " + probe_err);
    }

    rfb::RfbServer server(capturer, injector, "DeviceOps Windows Desktop");
    tunnel::TunnelClient client(tunnel_cfg, server, [&badge](bool on) {
        std::string badge_err;
        if (on) {
            if (!badge.show(badge_err)) {
                LOG_WARN("RemoteDesktopRuntime: BADGE on failed: " + badge_err);
            }
        } else {
            badge.hide();
        }
    });
    LOG_INFO(std::string("RemoteDesktopRuntime: starting tunnel client, stop=") +
             (stop.load() ? "true" : "false"));
    const bool ok = client.run(stop, err);
    LOG_INFO(std::string("RemoteDesktopRuntime: tunnel client returned ok=") +
             (ok ? "true" : "false") + " stop=" + (stop.load() ? "true" : "false") +
             " err=" + err);
    return ok;
#else
    err = "remote desktop runtime is only implemented for Windows in Phase 2 M4";
    return false;
#endif
}

struct RemoteDesktopRuntime::Impl {
    explicit Impl(RemoteDesktopRuntimeConfig c) : config(std::move(c)) {}

    RemoteDesktopRuntimeConfig config;
    std::atomic<bool> stop{false};
    std::thread worker;
#ifdef _WIN32
    PROCESS_INFORMATION child{};
#endif
};

RemoteDesktopRuntime::RemoteDesktopRuntime(RemoteDesktopRuntimeConfig config)
    : impl_(new Impl(std::move(config))) {}

RemoteDesktopRuntime::~RemoteDesktopRuntime() {
    stop();
}

bool RemoteDesktopRuntime::start(std::string& err) {
    if (!impl_->config.enabled()) {
        return true;
    }
    if (running()) {
        return true;
    }
    impl_->stop.store(false);

#ifdef _WIN32
    if (impl_->config.launch_active_session && windows::currentProcessIsSessionZero()) {
        const std::string exe = windows::currentExecutablePath();
        if (exe.empty()) {
            err = "failed to resolve current executable path";
            return false;
        }
        std::wstring cmd = windows::quoteArg(exe) + L" --remote-desktop-child --rd-relay " +
                           windows::quoteArg(impl_->config.relay_endpoint) + L" --rd-device " +
                           windows::quoteArg(impl_->config.device_id) + L" --rd-token " +
                           windows::quoteArg(impl_->config.token);
        if (impl_->config.insecure_tls) {
            cmd += L" --rd-insecure";
        }
        if (!impl_->config.server_name.empty()) {
            cmd += L" --rd-server-name " + windows::quoteArg(impl_->config.server_name);
        }
        if (!impl_->config.child_log_path.empty()) {
            cmd += L" --rd-log " + windows::quoteArg(impl_->config.child_log_path);
        }
        bool used_elevated_token = false;
        if (!windows::launchInActiveConsoleSessionElevatedHidden(cmd, impl_->child, used_elevated_token, err)) {
            return false;
        }
        LOG_INFO(std::string("RemoteDesktopRuntime: launched child in active session elevated_token=") +
                 (used_elevated_token ? "yes" : "no"));
        const HANDLE child_process = impl_->child.hProcess;
        std::thread([child_process]() {
            DWORD wait = WaitForSingleObject(child_process, 5000);
            if (wait == WAIT_OBJECT_0) {
                DWORD code = 0;
                if (GetExitCodeProcess(child_process, &code)) {
                    LOG_ERROR("RemoteDesktopRuntime: child exited early, code=" + std::to_string(code));
                } else {
                    LOG_ERROR("RemoteDesktopRuntime: child exited early, code unavailable");
                }
            }
        }).detach();
        return true;
    }
#endif

    impl_->worker = std::thread([this]() {
        std::string run_err;
        if (!runRemoteDesktopChild(impl_->config, impl_->stop, run_err)) {
            LOG_ERROR("RemoteDesktopRuntime: stopped with error: " + run_err);
        }
    });
    LOG_INFO("RemoteDesktopRuntime: started in-process worker");
    return true;
}

void RemoteDesktopRuntime::stop() {
    impl_->stop.store(true);
    if (impl_->worker.joinable()) {
        impl_->worker.join();
    }
#ifdef _WIN32
    if (impl_->child.hProcess) {
        TerminateProcess(impl_->child.hProcess, 0);
        CloseHandle(impl_->child.hProcess);
        CloseHandle(impl_->child.hThread);
        ZeroMemory(&impl_->child, sizeof(impl_->child));
    }
#endif
}

bool RemoteDesktopRuntime::running() const {
    if (impl_->worker.joinable()) {
        return true;
    }
#ifdef _WIN32
    if (impl_->child.hProcess) {
        DWORD code = STILL_ACTIVE;
        if (GetExitCodeProcess(impl_->child.hProcess, &code) && code == STILL_ACTIVE) {
            return true;
        }
    }
#endif
    return false;
}

}  // namespace device_agent::remotedesktop
