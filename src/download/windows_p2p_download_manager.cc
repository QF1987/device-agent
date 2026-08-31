// ============================================================
// download/windows_p2p_download_manager.cc
// ============================================================
// 见 windows_p2p_download_manager.h 的总体说明（ADR-20260831-01 · B1）。
// ============================================================

#include "download/windows_p2p_download_manager.h"

#include "config/p2p_config_store.h"
#include "logger/logger.h"

#ifdef _WIN32
#include "download/windows_download_manager.h"
#endif

#include <algorithm>
#include <atomic>
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
    // 与 run() 的发布临界区互斥：cancel 要么发生在发布前（latch 生效，run
    // 不启动），要么观察到 active_ 并送达 request_cancel（含 job 尚未启动
    // 的窗口——由 job 自身取消标志兜底，见 WdmRunJob）。
    std::lock_guard<std::mutex> lock(mu_);
    cancel_requested_ = true;
    if (active_) {
        active_->request_cancel();
    }
}

void WindowsHttpFallbackAdapter::reset() {
    std::lock_guard<std::mutex> lock(mu_);
    cancel_requested_ = false;
}

#ifdef DEVICE_AGENT_TESTING
void WindowsHttpFallbackAdapter::set_job_factory_for_test(
    std::function<std::unique_ptr<RunJob>(const std::string&,
                                          const std::string&)>
        factory) {
    std::lock_guard<std::mutex> lock(mu_);
    job_factory_ = std::move(factory);
}

void WindowsHttpFallbackAdapter::set_start_gate_for_test(
    std::function<void()> gate) {
    std::lock_guard<std::mutex> lock(mu_);
    start_gate_ = std::move(gate);
}
#endif

std::unique_ptr<WindowsHttpFallbackAdapter::RunJob>
WindowsHttpFallbackAdapter::create_job(const std::string& url,
                                       const std::string& output_path) {
#ifdef DEVICE_AGENT_TESTING
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (job_factory_) {
            return job_factory_(url, output_path);
        }
    }
#endif
#ifdef _WIN32
    struct WdmRunJob : RunJob {
        // job 级取消标志：request_cancel 先于 execute 到达时，
        // execute 立即失败且不产生任何网络 I/O（WDM.download 会清自己的
        // cancel 标志，故不能只依赖 WDM）。
        std::atomic<bool> job_cancelled_{false};
        WindowsDownloadManager wdm;
        DownloadRequest req;

        explicit WdmRunJob(const std::string& url,
                           const std::string& output_path) {
            const auto sep = output_path.find_last_of("/\\");
            if (sep == std::string::npos) {
                req.file_id = output_path;
            } else {
                req.dest_path = output_path.substr(0, sep);
                req.file_id = output_path.substr(sep + 1);
            }
            req.url = url;
        }

        bool execute(std::string& error) override {
            if (job_cancelled_.load()) {
                error = "windows http fallback cancelled";
                return false;
            }
            std::mutex done_mu;
            std::condition_variable done_cv;
            bool done = false;
            bool ok = false;
            std::string child_error;
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
            error = child_error;
            return ok;
        }

        void request_cancel() override {
            job_cancelled_.store(true);
            wdm.cancel();
        }
    };
    return std::unique_ptr<RunJob>(new WdmRunJob(url, output_path));
#else
    (void)url;
    (void)output_path;
    struct FailClosedRunJob : RunJob {
        bool execute(std::string& error) override {
            error = "windows http fallback is not available on this platform";
            return false;
        }
        void request_cancel() override {}
    };
    return std::unique_ptr<RunJob>(new FailClosedRunJob());
#endif
}

bool WindowsHttpFallbackAdapter::run(const std::string& url,
                                     const std::string& output_path,
                                     std::string& error) {
    {
        // 快路径：latch 已置时不构造 job（正确性由下方原子临界区保证）。
        std::lock_guard<std::mutex> lock(mu_);
        if (cancel_requested_) {
            error = "windows http fallback cancelled";
            return false;
        }
    }
    std::unique_ptr<RunJob> job = create_job(url, output_path);
    {
        // RV-20260831-WIN-P2P-B1-01：latch 复查与 active 发布必须在同一
        // 互斥临界区。线性化保证：cancel 与 run 的任一顺序要么让 run 在
        // 发布前观察到已取消（不启动），要么让 cancel 观察到 active_ 并
        // 送达 request_cancel；不存在两者之间的漏取消窗口。
        std::lock_guard<std::mutex> lock(mu_);
        if (cancel_requested_) {
            error = "windows http fallback cancelled";
            return false;
        }
        active_ = std::move(job);
    }
#ifdef DEVICE_AGENT_TESTING
    if (start_gate_) {
        start_gate_();  // 确定性窗口入口：已发布、job 未启动
    }
#endif
    const bool ok = active_->execute(error);
    {
        std::lock_guard<std::mutex> lock(mu_);
        active_.reset();
    }
    return ok;
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
