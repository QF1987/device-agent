#include "download/p2p_download_manager.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

namespace {

struct Args {
    std::string torrent;
    std::string url;
    std::string dest;
    std::string sha256;
    int64_t file_size = 0;
    int keep_seeding_seconds = 30;
};

void usage(const char* argv0) {
    std::cerr
        << "Usage: " << argv0 << " --torrent <path> --dest <dir> "
        << "[--url <web-seed>] [--sha256 <hex>] [--file-size <bytes>] "
        << "[--keep-seeding-seconds <seconds>]\n";
}

bool parse_args(int argc, char** argv, Args& args) {
    for (int i = 1; i < argc; ++i) {
        const std::string key = argv[i];
        auto take_value = [&](std::string& out) -> bool {
            if (i + 1 >= argc) {
                return false;
            }
            out = argv[++i];
            return true;
        };

        if (key == "--torrent") {
            if (!take_value(args.torrent)) return false;
        } else if (key == "--url") {
            if (!take_value(args.url)) return false;
        } else if (key == "--dest") {
            if (!take_value(args.dest)) return false;
        } else if (key == "--sha256") {
            if (!take_value(args.sha256)) return false;
        } else if (key == "--file-size") {
            std::string value;
            if (!take_value(value)) return false;
            args.file_size = std::strtoll(value.c_str(), nullptr, 10);
        } else if (key == "--keep-seeding-seconds") {
            std::string value;
            if (!take_value(value)) return false;
            args.keep_seeding_seconds = std::atoi(value.c_str());
        } else if (key == "--help" || key == "-h") {
            return false;
        } else {
            std::cerr << "unknown argument: " << key << "\n";
            return false;
        }
    }

    return !args.torrent.empty() && !args.dest.empty();
}

}  // namespace

int main(int argc, char** argv) {
    Args args;
    if (!parse_args(argc, argv, args)) {
        usage(argv[0]);
        return 2;
    }

    device_agent::DownloadRequest req;
    req.torrent_url = args.torrent;
    req.url = args.url;
    req.dest_path = args.dest;
    req.expected_sha256 = args.sha256;
    req.file_size = args.file_size;

    std::mutex mu;
    std::condition_variable cv;
    std::atomic<bool> done{false};
    bool ok = false;
    std::string error;

    device_agent::P2PDownloadManager::Callbacks callbacks;
    callbacks.on_started = [](const device_agent::DownloadRequest&, const std::string& path) {
        std::cout << "RUNNER on_started path=" << path << std::endl;
    };
    callbacks.on_progress = [](const device_agent::DownloadRequest&,
                               const device_agent::DownloadProgress& progress) {
        std::cout << "RUNNER progress downloaded=" << progress.downloaded_bytes
                  << " total=" << progress.total_bytes
                  << " percent=" << progress.percent << std::endl;
    };
    callbacks.on_complete = [&](const device_agent::DownloadRequest&,
                                const std::string& path,
                                bool success,
                                const std::string& complete_error) {
        std::lock_guard<std::mutex> lock(mu);
        ok = success;
        error = complete_error;
        done.store(true);
        std::cout << "RUNNER on_complete success=" << (success ? "true" : "false")
                  << " path=" << path
                  << " error=" << complete_error << std::endl;
        cv.notify_one();
    };

    device_agent::P2PDownloadManager manager(
        callbacks,
        device_agent::P2PSeedingPolicy{
            std::chrono::seconds(args.keep_seeding_seconds),
            0.0,
        });

    manager.download(req, nullptr, nullptr);

    {
        std::unique_lock<std::mutex> lock(mu);
        if (!cv.wait_for(lock, std::chrono::minutes(5), [&] { return done.load(); })) {
            manager.cancel();
            std::cerr << "RUNNER timeout waiting for completion\n";
            return 3;
        }
    }

    if (!ok) {
        std::cerr << "RUNNER failed: " << error << "\n";
        return 1;
    }

    std::this_thread::sleep_for(std::chrono::seconds(args.keep_seeding_seconds));
    return 0;
}
