#include "observability/windows_observability.h"

#include <algorithm>
#include <climits>
#include <cwctype>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <vector>
#include <utility>

#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <windows.h>
#include <winternl.h>
#endif

namespace device_agent {
namespace observability {
namespace {

#ifdef _WIN32

std::string utf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                         static_cast<int>(value.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::string out(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        &out[0], size, nullptr, nullptr);
    return out;
}

std::optional<std::string> registry_string(HKEY root, const wchar_t* path,
                                           const wchar_t* name,
                                           std::uint32_t* error_code) {
    DWORD type = 0;
    DWORD bytes = 0;
    LONG status = RegGetValueW(root, path, name, RRF_RT_REG_SZ, &type,
                               nullptr, &bytes);
    if (status != ERROR_SUCCESS || bytes < sizeof(wchar_t)) {
        if (error_code != nullptr) {
            *error_code = static_cast<std::uint32_t>(status);
        }
        return std::nullopt;
    }
    std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1, L'\0');
    status = RegGetValueW(root, path, name, RRF_RT_REG_SZ, &type,
                          buffer.data(), &bytes);
    if (status != ERROR_SUCCESS) {
        if (error_code != nullptr) {
            *error_code = static_cast<std::uint32_t>(status);
        }
        return std::nullopt;
    }
    std::wstring value(buffer.data());
    while (!value.empty() && iswspace(value.back())) {
        value.pop_back();
    }
    const std::string converted = utf8(value);
    if (converted.empty()) {
        if (error_code != nullptr) {
            *error_code = ERROR_NO_DATA;
        }
        return std::nullopt;
    }
    return converted;
}

void add_error(RawObservation* raw, std::string stage, std::string message,
               std::uint32_t native_code) {
    raw->errors.push_back({std::move(stage), std::move(message), native_code});
}

std::uint64_t filetime_value(const FILETIME& value) {
    ULARGE_INTEGER out{};
    out.LowPart = value.dwLowDateTime;
    out.HighPart = value.dwHighDateTime;
    return out.QuadPart;
}

std::optional<float> cpu_percent(std::uint32_t* error_code) {
    FILETIME idle{}, kernel{}, user{};
    if (!GetSystemTimes(&idle, &kernel, &user)) {
        if (error_code != nullptr) {
            *error_code = GetLastError();
        }
        return std::nullopt;
    }

    static std::mutex mu;
    static bool initialized = false;
    static std::uint64_t previous_idle = 0;
    static std::uint64_t previous_kernel = 0;
    static std::uint64_t previous_user = 0;

    const std::uint64_t current_idle = filetime_value(idle);
    const std::uint64_t current_kernel = filetime_value(kernel);
    const std::uint64_t current_user = filetime_value(user);
    std::lock_guard<std::mutex> lock(mu);
    if (!initialized) {
        initialized = true;
        previous_idle = current_idle;
        previous_kernel = current_kernel;
        previous_user = current_user;
        if (error_code != nullptr) {
            *error_code = ERROR_RETRY;
        }
        return std::nullopt;
    }
    const std::uint64_t idle_delta = current_idle - previous_idle;
    const std::uint64_t kernel_delta = current_kernel - previous_kernel;
    const std::uint64_t user_delta = current_user - previous_user;
    previous_idle = current_idle;
    previous_kernel = current_kernel;
    previous_user = current_user;
    const std::uint64_t total = kernel_delta + user_delta;
    if (total == 0) {
        if (error_code != nullptr) {
            *error_code = ERROR_RETRY;
        }
        return std::nullopt;
    }
    const double busy = static_cast<double>(total - std::min(idle_delta, total));
    return static_cast<float>(std::clamp(busy * 100.0 / total, 0.0, 100.0));
}

std::optional<std::string> os_version(std::uint32_t* error_code) {
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    auto rtl_get_version = ntdll == nullptr
        ? nullptr
        : reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"));
    if (rtl_get_version == nullptr) {
        if (error_code != nullptr) {
            *error_code = GetLastError();
        }
        return std::nullopt;
    }
    RTL_OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    const LONG status = rtl_get_version(&version);
    if (status != 0) {
        if (error_code != nullptr) {
            *error_code = static_cast<std::uint32_t>(status);
        }
        return std::nullopt;
    }
    return std::to_string(version.dwMajorVersion) + "." +
           std::to_string(version.dwMinorVersion) + "." +
           std::to_string(version.dwBuildNumber);
}

std::optional<std::string> architecture() {
    SYSTEM_INFO info{};
    GetNativeSystemInfo(&info);
    switch (info.wProcessorArchitecture) {
        case PROCESSOR_ARCHITECTURE_AMD64: return "amd64";
        case PROCESSOR_ARCHITECTURE_ARM64: return "arm64";
        case PROCESSOR_ARCHITECTURE_INTEL: return "386";
        default: return std::nullopt;
    }
}

std::string network_cidr(const sockaddr_in* address, unsigned long prefix) {
    if (address == nullptr || prefix > 32) {
        return {};
    }
    const std::uint32_t host_ip = ntohl(address->sin_addr.s_addr);
    const std::uint32_t mask = prefix == 0 ? 0 : 0xffffffffu << (32 - prefix);
    in_addr network{};
    network.s_addr = htonl(host_ip & mask);
    char text[INET_ADDRSTRLEN] = {};
    if (InetNtopA(AF_INET, &network, text, sizeof(text)) == nullptr) {
        return {};
    }
    return std::string(text) + "/" + std::to_string(prefix);
}

void collect_network(RawObservation* raw) {
    ULONG bytes = 16 * 1024;
    std::vector<unsigned char> storage(bytes);
    auto* addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data());
    ULONG status = GetAdaptersAddresses(
        AF_INET, GAA_FLAG_INCLUDE_PREFIX | GAA_FLAG_INCLUDE_GATEWAYS,
        nullptr, addresses, &bytes);
    if (status == ERROR_BUFFER_OVERFLOW) {
        storage.resize(bytes);
        addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data());
        status = GetAdaptersAddresses(
            AF_INET, GAA_FLAG_INCLUDE_PREFIX | GAA_FLAG_INCLUDE_GATEWAYS,
            nullptr, addresses, &bytes);
    }
    if (status != NO_ERROR) {
        add_error(raw, "network.adapters", "GetAdaptersAddresses failed", status);
        return;
    }

    std::uint64_t rx_total = 0;
    std::uint64_t tx_total = 0;
    bool counters_present = false;
    bool selected = false;
    bool selected_has_gateway = false;
    for (auto* adapter = addresses; adapter != nullptr; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp ||
            adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK ||
            adapter->FirstUnicastAddress == nullptr) {
            continue;
        }
        MIB_IF_ROW2 row{};
        row.InterfaceLuid = adapter->Luid;
        const NETIO_STATUS row_status = GetIfEntry2(&row);
        if (row_status == NO_ERROR) {
            rx_total += row.InOctets;
            tx_total += row.OutOctets;
            counters_present = true;
        } else {
            add_error(raw, "network.counters", "GetIfEntry2 failed", row_status);
        }

        const bool has_gateway = adapter->FirstGatewayAddress != nullptr;
        if (selected && (selected_has_gateway || !has_gateway)) {
            continue;
        }
        const auto* socket_address = adapter->FirstUnicastAddress->Address.lpSockaddr;
        if (socket_address == nullptr || socket_address->sa_family != AF_INET) {
            continue;
        }
        const auto* address = reinterpret_cast<const sockaddr_in*>(socket_address);
        char text[INET_ADDRSTRLEN] = {};
        if (InetNtopA(AF_INET, &address->sin_addr, text, sizeof(text)) == nullptr) {
            continue;
        }
        raw->lan_ip = text;
        const std::string cidr = network_cidr(
            address, adapter->FirstUnicastAddress->OnLinkPrefixLength);
        if (!cidr.empty()) {
            raw->lan_cidr = cidr;
        }
        if (adapter->IfType == IF_TYPE_IEEE80211) {
            raw->net_type = terminal_agent::v1::WIFI;
        } else if (adapter->IfType == IF_TYPE_ETHERNET_CSMACD) {
            raw->net_type = terminal_agent::v1::ETHERNET;
        } else if (const auto mapped = network_type_from_if_type(adapter->IfType);
                   mapped.has_value()) {
            // ADR-20260831-01 B2：WWAN 蜂窝适配器 → proto CELLULAR。
            raw->net_type = *mapped;
        } else {
            raw->net_type.reset();
            add_error(raw, "network.type", "active adapter type unsupported",
                      adapter->IfType);
        }
        selected = true;
        selected_has_gateway = has_gateway;
    }
    if (!selected) {
        add_error(raw, "network.active_adapter", "no active IPv4 adapter", ERROR_NO_DATA);
    }
    if (counters_present) {
        raw->network_rx_bytes = static_cast<std::int64_t>(
            std::min<std::uint64_t>(rx_total, LLONG_MAX));
        raw->network_tx_bytes = static_cast<std::int64_t>(
            std::min<std::uint64_t>(tx_total, LLONG_MAX));
    }
}

#endif

}  // namespace

#ifdef _WIN32
std::optional<terminal_agent::v1::NetworkType> network_type_from_if_type(
    std::uint32_t if_type) {
    if (if_type == IF_TYPE_IEEE80211) {
        return terminal_agent::v1::WIFI;
    }
    if (if_type == IF_TYPE_ETHERNET_CSMACD) {
        return terminal_agent::v1::ETHERNET;
    }
    // ADR-20260831-01 B2：WWAN 蜂窝适配器归入 CELLULAR（此前落 unknown）。
    if (if_type == IF_TYPE_WWANPP) {
        return terminal_agent::v1::CELLULAR;
    }
#ifdef IF_TYPE_WWANPP2
    if (if_type == IF_TYPE_WWANPP2) {
        return terminal_agent::v1::CELLULAR;
    }
#endif
    return std::nullopt;
}
#endif

ObservabilitySnapshot collect_windows_observability() {
    RawObservation raw;
#ifdef _WIN32
    raw.platform = "win";
    raw.os_arch = architecture();
    if (!raw.os_arch.has_value()) {
        add_error(&raw, "inventory.arch", "unsupported processor architecture", ERROR_NOT_SUPPORTED);
    }

    wchar_t hostname[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD hostname_size = MAX_COMPUTERNAME_LENGTH + 1;
    if (GetComputerNameW(hostname, &hostname_size)) {
        raw.hostname = utf8(std::wstring(hostname, hostname_size));
    } else {
        add_error(&raw, "inventory.hostname", "GetComputerNameW failed", GetLastError());
    }

    std::uint32_t code = 0;
    raw.os_name = registry_string(
        HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
        L"ProductName", &code);
    if (!raw.os_name.has_value()) {
        add_error(&raw, "inventory.os_name", "ProductName unavailable", code);
    }
    raw.os_version = os_version(&code);
    if (!raw.os_version.has_value()) {
        add_error(&raw, "inventory.os_version", "RtlGetVersion failed", code);
    }
    raw.cpu_model = registry_string(
        HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
        L"ProcessorNameString", &code);
    if (!raw.cpu_model.has_value()) {
        add_error(&raw, "inventory.cpu_model", "ProcessorNameString unavailable", code);
    }
    const DWORD logical_count = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (logical_count > 0 && logical_count <= static_cast<DWORD>(INT_MAX)) {
        raw.cpu_logical_count = static_cast<std::int32_t>(logical_count);
    } else {
        add_error(&raw, "inventory.cpu_logical_count", "GetActiveProcessorCount failed",
                  GetLastError());
    }

    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory)) {
        raw.memory_total_bytes = static_cast<std::int64_t>(
            std::min<std::uint64_t>(memory.ullTotalPhys, LLONG_MAX));
        raw.memory_percent = static_cast<float>(memory.dwMemoryLoad);
    } else {
        add_error(&raw, "memory", "GlobalMemoryStatusEx failed", GetLastError());
    }

    wchar_t windows_directory[MAX_PATH] = {};
    const UINT directory_length = GetWindowsDirectoryW(windows_directory, MAX_PATH);
    if (directory_length > 0 && directory_length < MAX_PATH) {
        wchar_t root[] = L"C:\\";
        root[0] = windows_directory[0];
        ULARGE_INTEGER free_bytes{}, total_bytes{}, total_free_bytes{};
        if (GetDiskFreeSpaceExW(root, &free_bytes, &total_bytes, &total_free_bytes) &&
            total_bytes.QuadPart > 0) {
            raw.system_disk_total_bytes = static_cast<std::int64_t>(
                std::min<std::uint64_t>(total_bytes.QuadPart, LLONG_MAX));
            raw.system_disk_percent = static_cast<float>(
                (total_bytes.QuadPart - total_free_bytes.QuadPart) * 100.0 /
                total_bytes.QuadPart);
        } else {
            add_error(&raw, "disk", "GetDiskFreeSpaceExW failed", GetLastError());
        }
    } else {
        add_error(&raw, "disk.system_root", "GetWindowsDirectoryW failed", GetLastError());
    }

    raw.cpu_percent = cpu_percent(&code);
    if (!raw.cpu_percent.has_value()) {
        add_error(&raw, "cpu.sample", "CPU delta sample not ready", code);
    }
    raw.uptime_seconds = static_cast<std::int32_t>(
        std::min<ULONGLONG>(GetTickCount64() / 1000, INT_MAX));
    collect_network(&raw);
#else
    raw.errors.push_back({"platform", "Windows collector unavailable on this build", 0});
#endif
    return finalize_observation(std::move(raw));
}

}  // namespace observability
}  // namespace device_agent
