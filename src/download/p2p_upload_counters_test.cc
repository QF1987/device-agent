#include "download/p2p_upload_counters.h"

#include <cstdlib>
#include <iostream>

// 蜂窝守门分桶单测(ADR-20260612-01 D2):
// WiFi 上传只进总桶;切蜂窝后的上传同时进蜂窝桶;非正增量忽略。
// 核心场景:WiFi 上传 → 切蜂窝 → 蜂窝上传,蜂窝桶必须只含后者。

namespace {

int g_failures = 0;

void expect_eq(long long got, long long want, const char* what) {
    if (got != want) {
        std::cerr << "FAIL: " << what << " got=" << got << " want=" << want << "\n";
        ++g_failures;
    }
}

}  // namespace

int main() {
    using device_agent::NetworkType;

    device_agent::reset_p2p_upload_counters_for_test();

    // WiFi 期间上传 1000:只进总桶
    device_agent::accumulate_p2p_upload(1000, NetworkType::WIFI);
    auto c = device_agent::p2p_upload_counters();
    expect_eq(c.total, 1000, "wifi total");
    expect_eq(c.cellular, 0, "wifi must not touch cellular bucket");

    // 切到蜂窝后上传 300:两桶都加
    device_agent::accumulate_p2p_upload(300, NetworkType::CELLULAR);
    c = device_agent::p2p_upload_counters();
    expect_eq(c.total, 1300, "cellular adds total");
    expect_eq(c.cellular, 300, "cellular bucket only counts cellular-period delta");

    // 非正增量忽略(采样回卷/无变化)
    device_agent::accumulate_p2p_upload(0, NetworkType::CELLULAR);
    device_agent::accumulate_p2p_upload(-50, NetworkType::CELLULAR);
    c = device_agent::p2p_upload_counters();
    expect_eq(c.total, 1300, "non-positive delta ignored (total)");
    expect_eq(c.cellular, 300, "non-positive delta ignored (cellular)");

    // 其它网络类型(NONE/OTHER)只进总桶
    device_agent::accumulate_p2p_upload(200, NetworkType::OTHER);
    c = device_agent::p2p_upload_counters();
    expect_eq(c.total, 1500, "other adds total");
    expect_eq(c.cellular, 300, "other must not touch cellular bucket");

    if (g_failures > 0) {
        std::cerr << g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "p2p_upload_counters_test: all assertions passed\n";
    return 0;
}
