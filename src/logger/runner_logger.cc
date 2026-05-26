#include "logger/runner_logger.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <ostream>
#include <sstream>

namespace device_agent {

RunnerLogger::RunnerLogger(Format format, std::string runner_id, std::ostream& out)
    : format_(format),
      runner_id_(std::move(runner_id)),
      out_(out) {}

void RunnerLogger::log(
        const std::string& event,
        const std::vector<std::pair<std::string, std::string>>& payload) {
    if (format_ == Format::Json) {
        out_ << "{\"runner_id\":\"" << escape_json(runner_id_)
             << "\",\"timestamp\":\"" << escape_json(timestamp())
             << "\",\"event\":\"" << escape_json(event)
             << "\",\"payload\":{";
        for (std::size_t i = 0; i < payload.size(); ++i) {
            if (i > 0) {
                out_ << ",";
            }
            out_ << "\"" << escape_json(payload[i].first)
                 << "\":\"" << escape_json(payload[i].second) << "\"";
        }
        out_ << "}}" << std::endl;
        return;
    }

    out_ << "timestamp=" << timestamp()
         << " runner_id=" << runner_id_
         << " event=" << event;
    for (const auto& item : payload) {
        out_ << " " << item.first << "=" << item.second;
    }
    out_ << std::endl;
}

std::string RunnerLogger::timestamp() const {
    const auto now = std::chrono::system_clock::now();
    const std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&tt, &tm);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

std::string RunnerLogger::escape_json(const std::string& value) const {
    std::string out;
    out.reserve(value.size());
    for (const char c : value) {
        switch (c) {
            case '\\':
                out += "\\\\";
                break;
            case '"':
                out += "\\\"";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                out.push_back(c);
                break;
        }
    }
    return out;
}

}  // namespace device_agent
