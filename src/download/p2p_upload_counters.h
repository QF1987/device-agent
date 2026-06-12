#pragma once

// ============================================================
// p2p_upload_counters.h - P2P 做种上传分桶计数(蜂窝守门,ADR-20260612-01)
// ============================================================
// 进程级累计计数:总桶恒加;蜂窝桶仅在「采样时刻网络为 CELLULAR」时加。
// 分桶必须在 device 侧做 —— server 端用"当前网络类型"反推会把
// 「WiFi 上传完成后切到蜂窝」的设备误判为蜂窝上传(D2)。
// 独立轻头文件(不依赖 libtorrent),供 device_client 周期状态上报读取。
// ============================================================

#include "download/network_policy.h"

#include <cstdint>

namespace device_agent {

struct P2PUploadCounters {
    std::int64_t total = 0;     // P2P 做种累计上传字节(本进程启动以来)
    std::int64_t cellular = 0;  // 其中蜂窝期间的上传字节(守门指标,正常恒 0)
};

// 读取当前累计值(线程安全)。
P2PUploadCounters p2p_upload_counters();

// 累加一次采样增量:delta<=0 忽略;type==CELLULAR 时同时计入蜂窝桶。
void accumulate_p2p_upload(std::int64_t delta, NetworkType type);

// 测试用:清零两桶。
void reset_p2p_upload_counters_for_test();

}  // namespace device_agent
