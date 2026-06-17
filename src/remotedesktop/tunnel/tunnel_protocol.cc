#include "remotedesktop/tunnel/tunnel_protocol.h"

#include <sstream>

namespace device_agent::remotedesktop::tunnel {

namespace {

std::string trim_crlf(std::string line) {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
        line.pop_back();
    }
    return line;
}

}  // namespace

std::string writeFrame(const std::vector<std::string>& fields) {
    std::string out;
    for (size_t i = 0; i < fields.size(); ++i) {
        if (i > 0) {
            out.push_back('\t');
        }
        out += fields[i];
    }
    out.push_back('\n');
    return out;
}

bool parseFrame(const std::string& line, std::vector<std::string>& fields, std::string& err) {
    if (line.size() > kMaxFrameLen) {
        err = "tunnel frame too long";
        return false;
    }
    fields.clear();
    std::string normalized = trim_crlf(line);
    if (normalized.empty()) {
        fields.emplace_back();
        return true;
    }
    std::stringstream ss(normalized);
    std::string field;
    while (std::getline(ss, field, '\t')) {
        fields.push_back(field);
    }
    return true;
}

std::string helloFrame(const std::string& device_id, const std::string& token, int screen_w, int screen_h) {
    std::vector<std::string> fields{"HELLO", device_id, token};
    if (screen_w > 0 && screen_h > 0) {
        fields.push_back(std::to_string(screen_w));
        fields.push_back(std::to_string(screen_h));
    }
    return writeFrame(fields);
}

std::string dataFrame(const std::string& stream_id, const std::string& device_id, const std::string& token) {
    return writeFrame({"DATA", stream_id, device_id, token});
}

std::string pingFrame() {
    return writeFrame({"PING"});
}

}  // namespace device_agent::remotedesktop::tunnel
