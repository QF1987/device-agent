// ============================================================
// reboot_state/reboot_state.cc - 重启状态管理实现
// ============================================================

#include "reboot_state/reboot_state.h"
#include "logger/logger.h"

#include <fstream>
#include <sstream>
#include <cstdio>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <dirent.h>

namespace device_agent {

// ─── 工具函数 ─────────────────────────────────────────────

// 确保目录存在
static bool ensure_dir(const std::string& path) {
    std::string dir = path.substr(0, path.find_last_of('/'));
    DIR* d = opendir(dir.c_str());
    if (d) {
        closedir(d);
        return true;
    }
    return mkdir(dir.c_str(), 0755) == 0;
}

// ─── RebootStateManager ────────────────────────────────────

RebootStateManager& RebootStateManager::instance() {
    static RebootStateManager mgr;
    return mgr;
}

std::string RebootStateManager::state_file_path() const {
    return state_file_;
}

bool RebootStateManager::write_pending(const std::string& command_id,
                                       const std::string& device_id,
                                       int64_t issued_at_ms) {
    if (state_file_.empty()) {
        // 默认路径
        state_file_ = "/var/run/device-agent/reboot_pending.json";
    }

    // 确保目录存在
    if (!ensure_dir(state_file_)) {
        LOG_ERROR("RebootStateManager: failed to create directory for " + state_file_);
        // 尝试用 /tmp 作为 fallback
        state_file_ = "/tmp/device-agent-reboot-pending.json";
    }

    std::ofstream ofs(state_file_);
    if (!ofs.is_open()) {
        LOG_ERROR("RebootStateManager: failed to open " + state_file_ + " for write");
        return false;
    }

    ofs << "{\n"
        << "  \"command_id\": \"" << command_id << "\",\n"
        << "  \"device_id\": \"" << device_id << "\",\n"
        << "  \"issued_at_ms\": " << issued_at_ms << ",\n"
        << "  \"status\": \"pending\"\n"
        << "}\n";
    ofs.close();

    LOG_INFO("RebootStateManager: written pending state to " + state_file_);
    return true;
}

bool RebootStateManager::read_pending(RebootPendingState& out) {
    if (state_file_.empty()) {
        state_file_ = "/var/run/device-agent/reboot_pending.json";
    }

    std::ifstream ifs(state_file_);
    if (!ifs.is_open()) {
        // 尝试 /tmp fallback
        state_file_ = "/tmp/device-agent-reboot-pending.json";
        ifs.open(state_file_);
        if (!ifs.is_open()) {
            return false;  // 没有 pending 状态
        }
    }

    std::stringstream ss;
    ss << ifs.rdbuf();
    ifs.close();

    std::string content = ss.str();

    // 简单解析 JSON（不用 jsoncpp）
    bool has_command_id = false, has_device_id = false, has_issued_at = false;

    size_t pos = content.find("\"command_id\"");
    if (pos != std::string::npos) {
        size_t colon = content.find(':', pos);
        size_t quote = content.find('"', colon);
        size_t end_quote = content.find('"', quote + 1);
        if (colon != std::string::npos && quote != std::string::npos && end_quote != std::string::npos) {
            out.command_id = content.substr(quote + 1, end_quote - quote - 1);
            has_command_id = true;
        }
    }

    pos = content.find("\"device_id\"");
    if (pos != std::string::npos) {
        size_t colon = content.find(':', pos);
        size_t quote = content.find('"', colon);
        size_t end_quote = content.find('"', quote + 1);
        if (colon != std::string::npos && quote != std::string::npos && end_quote != std::string::npos) {
            out.device_id = content.substr(quote + 1, end_quote - quote - 1);
            has_device_id = true;
        }
    }

    pos = content.find("\"issued_at_ms\"");
    if (pos != std::string::npos) {
        size_t colon = content.find(':', pos);
        size_t start = colon + 1;
        while (start < content.size() && (content[start] == ' ' || content[start] == '\t')) start++;
        size_t end = start;
        while (end < content.size() && content[end] >= '0' && content[end] <= '9') end++;
        if (end > start) {
            out.issued_at_ms = std::stoll(content.substr(start, end - start));
            has_issued_at = true;
        }
    }

    if (!has_command_id || !has_device_id || !has_issued_at) {
        LOG_WARN("RebootStateManager: incomplete pending state file");
        return false;
    }

    LOG_INFO("RebootStateManager: read pending state, command_id=" + out.command_id);
    return true;
}

void RebootStateManager::clear_pending() {
    if (state_file_.empty()) {
        state_file_ = "/var/run/device-agent/reboot_pending.json";
    }

    if (unlink(state_file_.c_str()) == 0) {
        LOG_INFO("RebootStateManager: cleared pending state");
    } else {
        // 尝试 /tmp fallback
        state_file_ = "/tmp/device-agent-reboot-pending.json";
        unlink(state_file_.c_str());
    }
}

bool RebootStateManager::has_pending() const {
    std::string path = state_file_.empty() ? "/var/run/device-agent/reboot_pending.json" : state_file_;
    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        path = "/tmp/device-agent-reboot-pending.json";
        ifs.open(path);
        if (!ifs.is_open()) {
            return false;
        }
    }
    return true;
}

}  // namespace device_agent
