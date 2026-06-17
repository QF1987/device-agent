#pragma once

#include <string>
#include <vector>

namespace device_agent::remotedesktop::tunnel {

constexpr size_t kMaxFrameLen = 4096;

std::string writeFrame(const std::vector<std::string>& fields);
bool parseFrame(const std::string& line, std::vector<std::string>& fields, std::string& err);

std::string helloFrame(const std::string& device_id, const std::string& token, int screen_w, int screen_h);
std::string dataFrame(const std::string& stream_id, const std::string& device_id, const std::string& token);
std::string pingFrame();

}  // namespace device_agent::remotedesktop::tunnel
