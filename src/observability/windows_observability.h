#pragma once

#include "observability/observability.h"

#ifdef _WIN32
#include <cstdint>

namespace device_agent {
namespace observability {

// IANA IfType → proto NetworkType（ADR-20260831-01 B2：WWAN → CELLULAR）。
// 802.11 → WIFI，Ethernet CSMA/CD → ETHERNET，WWANPP(/WWANPP2) → CELLULAR，
// 其它 → nullopt（NET_UNKNOWN 语义，fail closed）。仅测试与采集内部使用。
std::optional<terminal_agent::v1::NetworkType> network_type_from_if_type(
    std::uint32_t if_type);

}  // namespace observability
}  // namespace device_agent
#endif

namespace device_agent {
namespace observability {

ObservabilitySnapshot collect_windows_observability();

}  // namespace observability
}  // namespace device_agent
