// ============================================================
// download/windows_p2p_download_manager.cc
// ============================================================
// 见 windows_p2p_download_manager.h 的总体说明（ADR-20260831-01 · B1）。
// ============================================================

#include "download/windows_p2p_download_manager.h"

#include "config/p2p_config_store.h"
#include "logger/logger.h"

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <utility>
#include <vector>

namespace device_agent {
namespace {

bool is_http_url(const std::string& value) {
    return value.rfind("http://", 0) == 0 || value.rfind("https://", 0) == 0;
}

}  // namespace

// ─── WindowsHttpFallbackAdapter ──────────────────────────────

P2PDownloadManager::HttpFallback WindowsHttpFallbackAdapter::fallback() {
    // 绑定 this：适配器由 hybrid 持有 shared_ptr，生命周期覆盖全部请求。
    return [this](const std::string& url, const std::string& output_path,
                  std::string& error) {
        return run(url, output_path, error);
    };
}

void WindowsHttpFallbackAdapter::cancel() {
    std::lock_guard<std::mutex> lock(mu_);
    cancel_requested_ = true;
#ifdef _WIN32
    if (active_ != nullptr) {
        active_->cancel();
    }
#endif
}

void WindowsHttpFallbackAdapter::reset() {
    std::lock_guard<std::mutex> lock(mu_);
    cancel_requested_ = false;
}

bool WindowsHttpFallbackAdapter::run(const std::string& url,
                                     const std::string& output_path,
                                     std::string& error) {
#ifdef _WIN32
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (cancel_requested_) {
            error = "windows http fallback cancelled";
            return false;
        }
    }

    // 每次调用独立 WDM 实例：Range/.part/SHA256/原子改名语义与生产主路一致。
    WindowsDownloadManager wdm;
    {
        std::lock_guard<std::mutex> lock(mu_);
        active_ = &wdm;
    }

    std::mutex done_mu;
    std::condition_variable done_cv;
    bool done = false;
    bool ok = false;
    std::string child_error;

    DownloadRequest req;
    const auto sep = output_path.find_last_of("/\\");
    if (sep == std::string::npos) {
        req.file_id = output_path;
    } else {
        req.dest_path = output_path.substr(0, sep);
        req.file_id = output_path.substr(sep + 1);
    }
    req.url = url;

    wdm.download(req, nullptr,
                 [&](bool success, const std::string& err,
                     const DownloadCompletionTelemetry&) {
                     std::lock_guard<std::mutex> lock(done_mu);
                     done = true;
                     ok = success;
                     child_error = err;
                     done_cv.notify_all();
                 });
    {
        std::unique_lock<std::mutex> lock(done_mu);
        done_cv.wait(lock, [&] { return done; });
    }

    {
        std::lock_guard<std::mutex> lock(mu_);
        active_ = nullptr;
    }
    error = child_error;
    return ok;
#else
    (void)url;
    (void)output_path;
    error = "windows http fallback is not available on this platform";
    return false;
#endif
}

// ─── WindowsP2PDownloadManager ───────────────────────────────

WindowsP2PDownloadManager::WindowsP2PDownloadManager(
        std::shared_ptr<NetworkPolicy> network_policy,
        P2PDownloadManager::Callbacks p2p_callbacks,
        std::shared_ptr<IDownloadManager> http_manager,
        std::shared_ptr<WindowsHttpFallbackAdapter> http_adapter)
    : network_policy_(std::move(network_policy)),
      http_adapter_(http_adapter != nullptr ? std::move(http_adapter)
                                            : std::make_shared<WindowsHttpFallbackAdapter>()),
      http_manager_(std::move(http_manager)),
      p2p_(std::move(p2p_callbacks),
           P2PSeedingPolicy::alpha_defaults(),
           network_policy_,
           http_adapter_->fallback()) {
#ifdef _WIN32
    if (!http_manager_) {
        http_manager_ = std::make_shared<WindowsDownloadManager>();
    }
#endif
}

WindowsP2PDownloadManager::~WindowsP2PDownloadManager() {
    cancel();
}

bool WindowsP2PDownloadManager::should_use_p2p(const DownloadRequest& req) const {
    if (req.magnet_uri.empty() && req.torrent_url.empty()) {
        return false;
    }
    const auto cfg = P2PConfigStore::global_snapshot();
    if (!cfg.p2p_enabled) {
        return false;
    }
    if (cfg.min_file_size_mb_for_p2p > 0 && req.file_size > 0 &&
            req.file_size < static_cast<std::int64_t>(cfg.min_file_size_mb_for_p2p) * 1024 * 1024) {
        return false;
    }
    if (network_policy_) {
        const NetworkType net = network_policy_->current_type();
        if (net == NetworkType::CELLULAR && !cfg.cellular_download_enabled) {
            return false;
        }
    }
    return true;
}

void WindowsP2PDownloadManager::download(const DownloadRequest& req,
                                         ProgressCallback on_progress,
                                         CompleteCallback on_complete) {
    bool expected = false;
    if (!downloading_.compare_exchange_strong(expected, true)) {
        if (on_complete) {
            on_complete(false, "windows p2p download already active",
                        DownloadCompletionTelemetry{});
        }
        return;
    }
    cancel_requested_.store(false);
    http_adapter_->reset();
    join_worker();
    std::uint64_t gen = 0;
    {
        std::lock_guard<std::mutex> lock(mu_);
        gen = ++generation_;
        const bool use_p2p = should_use_p2p(req);
        worker_ = std::thread(&WindowsP2PDownloadManager::run_request, this, req,
                              std::move(on_progress), std::move(on_complete), gen,
                              use_p2p);
    }
}

void WindowsP2PDownloadManager::cancel() {
    cancel_requested_.store(true);
    {
        std::lock_guard<std::mutex> lock(mu_);
        ++generation_;  // 使在飞回调全部失效
    }
    http_adapter_->cancel();
    if (http_manager_) {
        http_manager_->cancel();
    }
    p2p_.cancel();
    join_worker();
    downloading_.store(false);
    cancel_requested_.store(false);
}

bool WindowsP2PDownloadManager::is_downloading() const {
    return downloading_.load();
}

void WindowsP2PDownloadManager::join_worker() {
    std::thread worker;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!worker_.joinable()) {
            return;
        }
        worker = std::move(worker_);
    }
    worker.join();
}

void WindowsP2PDownloadManager::run_request(DownloadRequest req,
                                            ProgressCallback on_progress,
                                            CompleteCallback on_complete,
                                            std::uint64_t generation,
                                            bool use_p2p) {
    bool ok = false;
    std::string error;
    DownloadCompletionTelemetry telemetry;

    auto stale = [this, generation]() {
        std::lock_guard<std::mutex> lock(mu_);
        return generation != generation_;
    };

    if (!use_p2p && !http_manager_) {
        error = "windows http manager unavailable";
    } else if (use_p2p) {
        std::mutex done_mu;
        std::condition_variable done_cv;
        bool done = false;
        bool child_ok = false;
        std::string child_error;
        DownloadCompletionTelemetry child_telemetry;

        p2p_.download(req, on_progress,
                      [&, generation](bool success, const std::string& err,
                                      const DownloadCompletionTelemetry& t) {
                          std::lock_guard<std::mutex> lock(done_mu);
                          done = true;
                          child_ok = success;
                          child_error = err;
                          child_telemetry = t;
                          done_cv.notify_all();
                      });
        {
            std::unique_lock<std::mutex> lock(done_mu);
            done_cv.wait(lock, [&] { return done; });
        }

        if (child_ok) {
            ok = true;
            telemetry = child_telemetry;
        } else if (stale()) {
            // 取消/被新请求替换：不再回退，统一以取消失败终态。
            error = "download cancelled";
        } else if (is_http_url(req.url) && http_manager_) {
            // P2P 失败 → 最多回退 HTTP 一次。
            LOG_WARN("WindowsP2PDownloadManager: p2p failed; falling back to http once: " +
                     child_error);
            std::mutex fb_mu;
            std::condition_variable fb_cv;
            bool fb_done = false;
            http_manager_->download(req, nullptr,
                                    [&](bool success, const std::string& err,
                                        const DownloadCompletionTelemetry& t) {
                                        std::lock_guard<std::mutex> lock(fb_mu);
                                        fb_done = true;
                                        ok = success;
                                        error = err;
                                        telemetry = t;
                                        fb_cv.notify_all();
                                    });
            {
                std::unique_lock<std::mutex> lock(fb_mu);
                fb_cv.wait(lock, [&] { return fb_done; });
            }
            if (stale()) {
                ok = false;
                error = "download cancelled";
            }
        } else {
            error = child_error;
        }
    } else {
        std::mutex done_mu;
        std::condition_variable done_cv;
        bool done = false;
        http_manager_->download(req, on_progress,
                                [&](bool success, const std::string& err,
                                    const DownloadCompletionTelemetry& t) {
                                    std::lock_guard<std::mutex> lock(done_mu);
                                    done = true;
                                    ok = success;
                                    error = err;
                                    telemetry = t;
                                    done_cv.notify_all();
                                });
        {
            std::unique_lock<std::mutex> lock(done_mu);
            done_cv.wait(lock, [&] { return done; });
        }
        if (stale()) {
            ok = false;
            error = "download cancelled";
        }
    }

    // 终端 completion 恰好一次（worker 统一发出）。
    if (stale()) {
        ok = false;
        error = "download cancelled";
    }
    if (on_complete) {
        on_complete(ok, ok ? std::string() : error, telemetry);
    }
    downloading_.store(false);
}

}  // namespace device_agent
