#include "download/p2p_download_manager.h"

#include "logger/logger.h"

#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/alert.hpp>
#include <libtorrent/alert_types.hpp>
#include <libtorrent/error_code.hpp>
#include <libtorrent/session.hpp>
#include <libtorrent/session_params.hpp>
#include <libtorrent/settings_pack.hpp>
#include <libtorrent/torrent_handle.hpp>
#include <libtorrent/torrent_info.hpp>
#include <libtorrent/torrent_status.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <vector>

namespace device_agent {
namespace {

std::string dirname_or_current(const std::string& path) {
    const auto slash = path.find_last_of('/');
    if (slash == std::string::npos) {
        return ".";
    }
    if (slash == 0) {
        return "/";
    }
    return path.substr(0, slash);
}

std::string resolve_save_path(const DownloadRequest& req) {
    if (!req.dest_path.empty()) {
        return req.dest_path;
    }
    return dirname_or_current(req.url);
}

lt::session make_session() {
    lt::settings_pack pack;
    pack.set_bool(lt::settings_pack::enable_dht, true);
    pack.set_bool(lt::settings_pack::enable_lsd, true);
    pack.set_bool(lt::settings_pack::enable_upnp, false);
    pack.set_bool(lt::settings_pack::enable_natpmp, false);
    pack.set_int(lt::settings_pack::connections_limit, 64);
    pack.set_int(lt::settings_pack::active_downloads, 1);

    const auto alert_mask =
        lt::alert_category::error |
        lt::alert_category::status |
        lt::alert_category::storage |
        lt::alert_category::tracker;
    pack.set_int(lt::settings_pack::alert_mask,
                 static_cast<int>(static_cast<std::uint32_t>(alert_mask)));

    lt::session_params params;
    params.settings = pack;
    return lt::session(std::move(params));
}

std::string alert_message(const lt::alert* alert) {
    return alert == nullptr ? std::string() : alert->message();
}

bool is_terminal_error(const lt::alert* alert) {
    return lt::alert_cast<lt::torrent_error_alert>(alert) != nullptr ||
           lt::alert_cast<lt::file_error_alert>(alert) != nullptr ||
           lt::alert_cast<lt::storage_moved_failed_alert>(alert) != nullptr ||
           lt::alert_cast<lt::metadata_failed_alert>(alert) != nullptr ||
           lt::alert_cast<lt::hash_failed_alert>(alert) != nullptr;
}

DownloadProgress make_progress(const lt::torrent_status& status,
                               int64_t fallback_total) {
    const int64_t total = status.total_wanted > 0
        ? status.total_wanted
        : std::max<int64_t>(fallback_total, 0);
    const int percent = std::max(0, std::min(100, status.progress_ppm / 10000));
    return DownloadProgress{
        status.total_wanted_done,
        total,
        percent,
    };
}

}  // namespace

P2PDownloadManager::~P2PDownloadManager() {
    cancel();
}

void P2PDownloadManager::download(const DownloadRequest& req,
                                  ProgressCallback on_progress,
                                  CompleteCallback on_complete) {
    if (req.url.empty()) {
        if (on_complete) {
            on_complete(false, "torrent path is empty");
        }
        return;
    }

    bool expected = false;
    if (!downloading_.compare_exchange_strong(expected, true)) {
        if (on_complete) {
            on_complete(false, "p2p download already active");
        }
        return;
    }

    cancel_requested_.store(false);
    join_worker();

    {
        std::lock_guard<std::mutex> lock(mu_);
        worker_ = std::thread(&P2PDownloadManager::run_download, this, req,
                              std::move(on_progress), std::move(on_complete));
    }
}

void P2PDownloadManager::cancel() {
    cancel_requested_.store(true);
    join_worker();
    downloading_.store(false);
    cancel_requested_.store(false);
}

bool P2PDownloadManager::is_downloading() const {
    return downloading_.load();
}

void P2PDownloadManager::join_worker() {
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

void P2PDownloadManager::run_download(DownloadRequest req,
                                      ProgressCallback on_progress,
                                      CompleteCallback on_complete) {
    bool success = false;
    std::string error;

    try {
        LOG_INFO("P2PDownloadManager: loading torrent " + req.url);

        lt::error_code ec;
        auto torrent = std::make_shared<lt::torrent_info>(req.url, ec);
        if (ec) {
            error = "failed to load torrent: " + ec.message();
        } else {
            lt::add_torrent_params params;
            params.ti = torrent;
            params.save_path = resolve_save_path(req);

            lt::session_proxy proxy;
            lt::session session = make_session();
            lt::torrent_handle handle = session.add_torrent(std::move(params), ec);
            if (ec) {
                error = "failed to add torrent: " + ec.message();
            } else {
                LOG_INFO("P2PDownloadManager: torrent added, save_path=" +
                         resolve_save_path(req));

                while (!cancel_requested_.load()) {
                    if (auto* alert = session.wait_for_alert(std::chrono::milliseconds(250))) {
                        std::vector<lt::alert*> alerts;
                        session.pop_alerts(&alerts);
                        for (const auto* item : alerts) {
                            if (lt::alert_cast<lt::torrent_finished_alert>(item) != nullptr) {
                                success = true;
                                break;
                            }
                            if (is_terminal_error(item)) {
                                error = alert_message(item);
                                break;
                            }
                        }
                    }

                    if (success || !error.empty()) {
                        break;
                    }

                    lt::torrent_status status = handle.status();
                    if (on_progress) {
                        on_progress(make_progress(status, req.file_size));
                    }
                    if (status.is_finished) {
                        success = true;
                        break;
                    }
                }

                if (cancel_requested_.load()) {
                    error = "download cancelled";
                }

                if (handle.is_valid()) {
                    handle.pause();
                    session.remove_torrent(handle);
                }
                proxy = session.abort();
            }
        }
    } catch (const std::exception& ex) {
        error = ex.what();
    } catch (...) {
        error = "unknown p2p download error";
    }

    downloading_.store(false);
    if (on_complete) {
        on_complete(success, success ? std::string() : error);
    }
}

}  // namespace device_agent
