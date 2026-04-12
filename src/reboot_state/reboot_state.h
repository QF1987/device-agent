// ============================================================
// reboot_state/reboot_state.h - 重启状态管理
// ============================================================
// 管理 device-agent 重启 pending 状态文件，用于 C+D 方案：
//
//   - reboot 前写 pending 状态到文件
//   - 子进程执行 reboot（成功=系统重启，失败=写 status 文件）
//   - 启动时检查 pending 状态，判断是否需要补报 failure
//
// 文件路径：/var/run/device-agent/reboot_pending.json
//
// 设计说明：
//   使用文件而非内存，因为进程会在 reboot 成功后被内核杀掉
//   文件能跨进程/跨重启保持状态
// ============================================================

#pragma once

#include <string>

namespace device_agent {

// ─── RebootPendingState ────────────────────────────────────
struct RebootPendingState {
    std::string command_id;   // 指令 UUID
    int64_t issued_at_ms;     // 下发时间（毫秒）
    std::string device_id;    // 设备 ID
};

// ─── RebootStateManager ────────────────────────────────────
class RebootStateManager {
public:
    // 获取单例
    static RebootStateManager& instance();

    // 写 pending 状态（fork 前调用）
    // 写入后返回 true
    bool write_pending(const std::string& command_id,
                       const std::string& device_id,
                       int64_t issued_at_ms);

    // 读 pending 状态（启动时调用）
    // 如果没有 pending 状态，返回 false
    bool read_pending(RebootPendingState& out);

    // 清理 pending 状态（启动时判断成功后调用）
    void clear_pending();

    // 检查是否存在 pending 状态
    bool has_pending() const;

    // 获取状态文件路径
    std::string state_file_path() const;

private:
    RebootStateManager() = default;

    // 状态文件路径（编译时确定）
    std::string state_file_;
};

}  // namespace device_agent
