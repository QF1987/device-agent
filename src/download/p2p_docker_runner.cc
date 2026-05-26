#include "download/p2p_download_manager.h"
#include "logger/runner_logger.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

struct Args {
    std::string torrent;
    std::string url;
    std::string dest;
    std::string sha256;
    std::string log_format = "text";
    std::string runner_id = "p2p-runner";
    std::string tracker_url;
    int64_t file_size = 0;
    int keep_seeding_seconds = 30;
    int peer_wait_seconds = 0;
    int retry_count = 0;
    int metric_port = 0;
    bool self_test = false;
};

void usage(const char* argv0) {
    std::cerr
        << "Usage: " << argv0 << " --torrent <path> --dest <dir> "
        << "[--url <web-seed>] [--sha256 <hex>] [--file-size <bytes>] "
        << "[--keep-seeding-seconds <seconds>] [--self-test] "
        << "[--log-format <json|text>] [--runner-id <id>] "
        << "[--tracker-url <url>] [--peer-wait-seconds <n>] "
        << "[--retry-count <n>] [--metric-port <n>]\n";
}

std::string env_or_empty(const char* name) {
    const char* value = std::getenv(name);
    return value == nullptr ? std::string() : std::string(value);
}

int env_int_or_default(const char* name, int fallback) {
    const std::string value = env_or_empty(name);
    return value.empty() ? fallback : std::atoi(value.c_str());
}

int64_t env_int64_or_default(const char* name, int64_t fallback) {
    const std::string value = env_or_empty(name);
    return value.empty() ? fallback : std::strtoll(value.c_str(), nullptr, 10);
}

void load_env(Args& args) {
    args.torrent = env_or_empty("P2P_RUNNER_TORRENT");
    args.url = env_or_empty("P2P_RUNNER_URL");
    args.dest = env_or_empty("P2P_RUNNER_DEST");
    args.sha256 = env_or_empty("P2P_RUNNER_SHA256");
    args.log_format = env_or_empty("P2P_RUNNER_LOG_FORMAT").empty()
        ? args.log_format
        : env_or_empty("P2P_RUNNER_LOG_FORMAT");
    args.runner_id = env_or_empty("P2P_RUNNER_ID").empty()
        ? args.runner_id
        : env_or_empty("P2P_RUNNER_ID");
    args.tracker_url = env_or_empty("P2P_TRACKER_URL");
    args.file_size = env_int64_or_default("P2P_RUNNER_FILE_SIZE", args.file_size);
    args.keep_seeding_seconds = env_int_or_default("P2P_RUNNER_KEEP_SEEDING_SECONDS",
                                                   args.keep_seeding_seconds);
    args.peer_wait_seconds = env_int_or_default("P2P_PEER_WAIT_SECONDS",
                                                args.peer_wait_seconds);
    args.retry_count = env_int_or_default("P2P_RETRY_COUNT", args.retry_count);
    args.metric_port = env_int_or_default("P2P_METRIC_PORT", args.metric_port);
}

bool take_value(int argc, char** argv, int& i, std::string& out) {
    if (i + 1 >= argc) {
        return false;
    }
    out = argv[++i];
    return true;
}

bool parse_args(int argc, char** argv, Args& args, std::string& error) {
    load_env(args);
    for (int i = 1; i < argc; ++i) {
        std::string key = argv[i];
        std::string inline_value;
        const auto equals = key.find('=');
        if (equals != std::string::npos) {
            inline_value = key.substr(equals + 1);
            key = key.substr(0, equals);
        }
        auto take = [&](std::string& out) -> bool {
            if (!inline_value.empty()) {
                out = inline_value;
                return true;
            }
            return take_value(argc, argv, i, out);
        };
        auto take_number = [&](int& out) -> bool {
            std::string value;
            if (!take(value)) {
                return false;
            }
            out = std::atoi(value.c_str());
            return true;
        };

        if (key == "--torrent") {
            if (!take(args.torrent)) return false;
        } else if (key == "--url") {
            if (!take(args.url)) return false;
        } else if (key == "--dest") {
            if (!take(args.dest)) return false;
        } else if (key == "--sha256") {
            if (!take(args.sha256)) return false;
        } else if (key == "--file-size") {
            std::string value;
            if (!take(value)) return false;
            args.file_size = std::strtoll(value.c_str(), nullptr, 10);
        } else if (key == "--keep-seeding-seconds") {
            if (!take_number(args.keep_seeding_seconds)) return false;
        } else if (key == "--self-test") {
            args.self_test = true;
        } else if (key == "--log-format") {
            if (!take(args.log_format)) return false;
        } else if (key == "--runner-id") {
            if (!take(args.runner_id)) return false;
        } else if (key == "--tracker-url") {
            if (!take(args.tracker_url)) return false;
        } else if (key == "--peer-wait-seconds") {
            if (!take_number(args.peer_wait_seconds)) return false;
        } else if (key == "--retry-count") {
            if (!take_number(args.retry_count)) return false;
        } else if (key == "--metric-port") {
            if (!take_number(args.metric_port)) return false;
        } else if (key == "--help" || key == "-h") {
            return false;
        } else {
            error = "unknown argument: " + key;
            return false;
        }
    }

    if (args.log_format != "text" && args.log_format != "json") {
        error = "invalid --log-format: " + args.log_format;
        return false;
    }
    if (args.self_test) {
        return true;
    }
    if (args.torrent.empty() || args.dest.empty()) {
        error = "missing required --torrent or --dest";
        return false;
    }
    return true;
}

device_agent::RunnerLogger::Format parse_log_format(const std::string& value) {
    return value == "json"
        ? device_agent::RunnerLogger::Format::Json
        : device_agent::RunnerLogger::Format::Text;
}

std::vector<std::pair<std::string, std::string>> reserved_arg_payload(const Args& args) {
    std::vector<std::pair<std::string, std::string>> payload;
    if (!args.tracker_url.empty()) {
        payload.emplace_back("tracker_url", args.tracker_url);
    }
    if (args.peer_wait_seconds > 0) {
        payload.emplace_back("peer_wait_seconds", std::to_string(args.peer_wait_seconds));
    }
    if (args.retry_count > 0) {
        payload.emplace_back("retry_count", std::to_string(args.retry_count));
    }
    if (args.metric_port > 0) {
        payload.emplace_back("metric_port", std::to_string(args.metric_port));
    }
    return payload;
}

}  // namespace

int main(int argc, char** argv) {
    Args args;
    std::string parse_error;
    if (!parse_args(argc, argv, args, parse_error)) {
        if (!parse_error.empty()) {
            std::cerr << parse_error << "\n";
        }
        usage(argv[0]);
        return 2;
    }

    std::streambuf* original_stdout = std::cout.rdbuf();
    std::ostream runner_stdout(original_stdout);
    if (args.log_format == "json") {
        std::cout.rdbuf(std::cerr.rdbuf());
    }
    device_agent::RunnerLogger logger(parse_log_format(args.log_format),
                                      args.runner_id,
                                      args.log_format == "json" ? runner_stdout : std::cout);
    logger.log("runner_config", {
        {"torrent", args.torrent},
        {"dest", args.dest},
        {"url", args.url},
        {"keep_seeding_seconds", std::to_string(args.keep_seeding_seconds)},
        {"file_size", std::to_string(args.file_size)},
    });
    const auto reserved_payload = reserved_arg_payload(args);
    if (!reserved_payload.empty()) {
        logger.log("reserved_cli_not_implemented", reserved_payload);
    }
    if (args.self_test) {
        logger.log("self_test_ok");
        return 0;
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
    callbacks.on_started = [&](const device_agent::DownloadRequest&, const std::string& path) {
        logger.log("download_started", {{"path", path}});
    };
    callbacks.on_progress = [&](const device_agent::DownloadRequest&,
                               const device_agent::DownloadProgress& progress) {
        logger.log("download_progress", {
            {"downloaded", std::to_string(progress.downloaded_bytes)},
            {"total", std::to_string(progress.total_bytes)},
            {"percent", std::to_string(progress.percent)},
        });
    };
    callbacks.on_complete = [&](const device_agent::DownloadRequest&,
                                const std::string& path,
                                bool success,
                                const std::string& complete_error) {
        std::lock_guard<std::mutex> lock(mu);
        ok = success;
        error = complete_error;
        done.store(true);
        logger.log("download_complete", {
            {"success", success ? "true" : "false"},
            {"path", path},
            {"error", complete_error},
        });
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
            logger.log("download_timeout", {{"timeout", "5m"}});
            return 3;
        }
    }

    if (!ok) {
        logger.log("runner_failed", {{"error", error}});
        return 1;
    }

    std::this_thread::sleep_for(std::chrono::seconds(args.keep_seeding_seconds));
    return 0;
}
