#include "download/p2p_upload_counters.h"

#include <atomic>

namespace device_agent {

namespace {
std::atomic<std::int64_t> g_p2p_upload_total{0};
std::atomic<std::int64_t> g_p2p_upload_cellular{0};
}  // namespace

P2PUploadCounters p2p_upload_counters() {
    P2PUploadCounters out;
    out.total = g_p2p_upload_total.load(std::memory_order_relaxed);
    out.cellular = g_p2p_upload_cellular.load(std::memory_order_relaxed);
    return out;
}

void accumulate_p2p_upload(std::int64_t delta, NetworkType type) {
    if (delta <= 0) {
        return;
    }
    g_p2p_upload_total.fetch_add(delta, std::memory_order_relaxed);
    if (type == NetworkType::CELLULAR) {
        g_p2p_upload_cellular.fetch_add(delta, std::memory_order_relaxed);
    }
}

void reset_p2p_upload_counters_for_test() {
    g_p2p_upload_total.store(0, std::memory_order_relaxed);
    g_p2p_upload_cellular.store(0, std::memory_order_relaxed);
}

}  // namespace device_agent
