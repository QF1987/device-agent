#pragma once

#include <string>

#ifndef DEVICE_AGENT_VERSION
#define DEVICE_AGENT_VERSION "0.0.0-dev"
#endif

namespace device_agent {

inline const std::string& agent_version() {
    static const std::string value(DEVICE_AGENT_VERSION);
    return value;
}

}  // namespace device_agent
