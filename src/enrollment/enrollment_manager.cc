#include "enrollment/enrollment_manager.h"
#include "version/build_info.h"

#include "logger/logger.h"
#include "terminal_agent/v1/service.grpc.pb.h"
#include "terminal_agent/v1/service.pb.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <random>
#include <sstream>
#include <thread>
#include <vector>

#include <grpcpp/grpcpp.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/utsname.h>
#include <unistd.h>
#endif

#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

namespace device_agent {
namespace enrollment {
namespace {

constexpr std::size_t kSha256BlockSize = 64;
constexpr std::size_t kSha256DigestSize = 32;

std::string trim(const std::string& s) {
    const auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    const auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

std::string lowercase(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

std::string hex_encode(const unsigned char* data, std::size_t len) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (std::size_t i = 0; i < len; ++i) {
        oss << std::setw(2) << static_cast<int>(data[i]);
    }
    return oss.str();
}

uint32_t rotr(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32 - n));
}

std::array<unsigned char, kSha256DigestSize> sha256_bytes(const std::string& input) {
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
        0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
        0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
        0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
        0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
        0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
        0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

    uint32_t h[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

    std::vector<unsigned char> msg(input.begin(), input.end());
    const uint64_t bit_len = static_cast<uint64_t>(msg.size()) * 8;
    msg.push_back(0x80);
    while ((msg.size() % 64) != 56) {
        msg.push_back(0);
    }
    for (int i = 7; i >= 0; --i) {
        msg.push_back(static_cast<unsigned char>((bit_len >> (i * 8)) & 0xff));
    }

    for (std::size_t chunk = 0; chunk < msg.size(); chunk += 64) {
        uint32_t w[64] = {};
        for (int i = 0; i < 16; ++i) {
            const std::size_t j = chunk + static_cast<std::size_t>(i) * 4;
            w[i] = (static_cast<uint32_t>(msg[j]) << 24) |
                   (static_cast<uint32_t>(msg[j + 1]) << 16) |
                   (static_cast<uint32_t>(msg[j + 2]) << 8) |
                   static_cast<uint32_t>(msg[j + 3]);
        }
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const uint32_t ch = (e & f) ^ ((~e) & g);
            const uint32_t temp1 = hh + s1 + ch + k[i] + w[i];
            const uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t temp2 = s0 + maj;
            hh = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }

    std::array<unsigned char, kSha256DigestSize> out{};
    for (int i = 0; i < 8; ++i) {
        out[static_cast<std::size_t>(i) * 4] = static_cast<unsigned char>((h[i] >> 24) & 0xff);
        out[static_cast<std::size_t>(i) * 4 + 1] = static_cast<unsigned char>((h[i] >> 16) & 0xff);
        out[static_cast<std::size_t>(i) * 4 + 2] = static_cast<unsigned char>((h[i] >> 8) & 0xff);
        out[static_cast<std::size_t>(i) * 4 + 3] = static_cast<unsigned char>(h[i] & 0xff);
    }
    return out;
}

std::string json_value(const std::string& content, const std::string& key) {
    const std::string q = "\"" + key + "\"";
    const auto pos = content.find(q);
    if (pos == std::string::npos) return "";
    const auto colon = content.find(':', pos);
    if (colon == std::string::npos) return "";
    const auto quote = content.find('"', colon);
    if (quote == std::string::npos) return "";
    const auto end = content.find('"', quote + 1);
    if (end == std::string::npos) return "";
    return content.substr(quote + 1, end - quote - 1);
}

std::string escape_json(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char ch : s) {
        if (ch == '\\' || ch == '"') out.push_back('\\');
        if (ch == '\n') {
            out += "\\n";
        } else {
            out.push_back(ch);
        }
    }
    return out;
}

bool ensure_parent_dir(const std::string& path, std::string* error) {
    std::error_code ec;
    const auto parent = std::filesystem::path(path).parent_path();
    if (parent.empty()) return true;
    std::filesystem::create_directories(parent, ec);
    if (ec) {
        if (error) *error = "create parent directory failed: " + parent.string() + ": " + ec.message();
        return false;
    }
    return true;
}

std::string env_or_empty(const char* key) {
    const char* value = std::getenv(key);
    return value == nullptr ? "" : std::string(value);
}

std::string read_first_line(const std::string& path) {
    std::ifstream in(path);
    std::string line;
    if (std::getline(in, line)) {
        return trim(line);
    }
    return "";
}

std::string run_command_first_value(const std::string& command) {
#ifdef _WIN32
    FILE* pipe = _popen(command.c_str(), "r");
#else
    FILE* pipe = popen(command.c_str(), "r");
#endif
    if (pipe == nullptr) return "";
    char buffer[256];
    std::string best;
    while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        const std::string line = trim(buffer);
        const std::string lower = lowercase(line);
        if (line.empty() || lower == "uuid" || lower == "serialnumber") {
            continue;
        }
        best = line;
        break;
    }
#ifdef _WIN32
    _pclose(pipe);
#else
    pclose(pipe);
#endif
    return best;
}

std::string run_command_output(const std::string& command) {
#ifdef _WIN32
    FILE* pipe = _popen(command.c_str(), "r");
#else
    FILE* pipe = popen(command.c_str(), "r");
#endif
    if (pipe == nullptr) return "";
    char buffer[512];
    std::string out;
    while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        out += buffer;
    }
#ifdef _WIN32
    _pclose(pipe);
#else
    pclose(pipe);
#endif
    return out;
}

void append_hardware_part(std::vector<std::string>* parts,
                          const std::string& key,
                          const std::string& value) {
    const std::string trimmed = trim(value);
    if (!trimmed.empty()) {
        parts->push_back(key + "=" + trimmed);
    }
}

std::string join_hardware_parts(const std::vector<std::string>& parts) {
    std::ostringstream oss;
    for (const auto& part : parts) {
        oss << part << "\n";
    }
    return normalize_hardware_material(oss.str());
}

std::string hostname() {
    char buf[256] = {};
#ifdef _WIN32
    DWORD size = sizeof(buf);
    if (::GetComputerNameA(buf, &size)) return std::string(buf, size);
#else
    if (gethostname(buf, sizeof(buf) - 1) == 0) return std::string(buf);
#endif
    return "";
}

std::string os_arch() {
#ifdef _WIN32
#if defined(_M_X64) || defined(__x86_64__)
    return "amd64";
#elif defined(_M_ARM64) || defined(__aarch64__)
    return "arm64";
#else
    return "windows";
#endif
#else
    struct utsname u {};
    if (uname(&u) == 0) return u.machine;
    return "";
#endif
}

std::string status_name(terminal_agent::v1::EnrollmentStatus status) {
    switch (status) {
        case terminal_agent::v1::ENROLLMENT_STATUS_ACTIVE: return "active";
        case terminal_agent::v1::ENROLLMENT_STATUS_PENDING: return "pending";
        case terminal_agent::v1::ENROLLMENT_STATUS_REJECTED: return "rejected";
        case terminal_agent::v1::ENROLLMENT_STATUS_REVOKED: return "revoked";
        default: return "unspecified";
    }
}

EnrollOnceResult enroll_once(const Config& config, const std::string& device_id,
                             const std::string& hardware_material,
                             const std::string& short_code) {
    EnrollOnceResult out;
    grpc::ChannelArguments args;
    args.SetMaxReceiveMessageSize(128 * 1024 * 1024);
    auto channel = grpc::CreateCustomChannel(config.server.server_address(),
                                             grpc::InsecureChannelCredentials(), args);
    auto stub = terminal_agent::v1::DeviceService::NewStub(channel);

    const auto now = std::chrono::system_clock::now();
    const auto unix_seconds = std::chrono::duration_cast<std::chrono::seconds>(
        now.time_since_epoch()).count();
    const std::string nonce = generate_nonce();

    terminal_agent::v1::EnrollRequest req;
    req.set_device_id(device_id);
    req.set_hardware_fingerprint_hash(sha256_hex(hardware_material));
    req.set_installer_key_id(config.enrollment.installer_key_id);
    req.set_installer_key_proof(installer_key_proof(
        config.enrollment.installer_key, device_id, nonce,
        static_cast<long long>(unix_seconds)));
    req.set_nonce(nonce);
    req.set_timestamp_unix_seconds(static_cast<long long>(unix_seconds));
    req.set_short_code(short_code);
    populate_enrollment_identity(&req);

    terminal_agent::v1::EnrollResponse resp;
    grpc::ClientContext ctx;
    ctx.set_wait_for_ready(false);
    ctx.set_deadline(std::chrono::system_clock::now() +
                     std::chrono::seconds(config.enrollment.timeout_seconds));

    const grpc::Status st = stub->Enroll(&ctx, req, &resp);
    if (!st.ok()) {
        out.retryable = true;
        out.retry_after_seconds = config.enrollment.retry_base_seconds;
        out.status = "network_error";
        out.message = st.error_message();
        return out;
    }

    out.status = status_name(resp.status());
    out.message = resp.message();
    out.retry_after_seconds = resp.retry_after_seconds();
    if (resp.status() == terminal_agent::v1::ENROLLMENT_STATUS_ACTIVE &&
        resp.accepted() && !resp.device_token().empty()) {
        out.active = true;
        out.credential.device_id = resp.device_id().empty() ? device_id : resp.device_id();
        out.credential.token = resp.device_token();
    } else if (resp.status() == terminal_agent::v1::ENROLLMENT_STATUS_REVOKED) {
        out.revoked = true;
        out.retryable = true;
    } else {
        out.retryable = true;
    }
    return out;
}

std::string resolved_token_file(const Config& config) {
    if (!config.enrollment.token_file.empty()) return config.enrollment.token_file;
    return default_token_file();
}

std::string resolved_status_file(const Config& config, const std::string& token_file) {
    if (!config.enrollment.status_file.empty()) return config.enrollment.status_file;
    return token_file + ".status";
}

}  // namespace

std::string normalize_hardware_material(const std::string& raw) {
    std::string out;
    bool last_space = false;
    for (unsigned char c : raw) {
        if (std::isspace(c)) {
            if (!last_space && !out.empty()) out.push_back(' ');
            last_space = true;
        } else {
            out.push_back(static_cast<char>(std::tolower(c)));
            last_space = false;
        }
    }
    return trim(out);
}

std::string sha256_hex(const std::string& data) {
    const auto digest = sha256_bytes(data);
    return hex_encode(digest.data(), digest.size());
}

std::string hmac_sha256_hex(const std::string& key, const std::string& data) {
    std::string k = key;
    if (k.size() > kSha256BlockSize) {
        const auto digest = sha256_bytes(k);
        k.assign(reinterpret_cast<const char*>(digest.data()), digest.size());
    }
    k.resize(kSha256BlockSize, '\0');
    std::string o_key_pad(kSha256BlockSize, '\0');
    std::string i_key_pad(kSha256BlockSize, '\0');
    for (std::size_t i = 0; i < kSha256BlockSize; ++i) {
        o_key_pad[i] = static_cast<char>(static_cast<unsigned char>(k[i]) ^ 0x5c);
        i_key_pad[i] = static_cast<char>(static_cast<unsigned char>(k[i]) ^ 0x36);
    }
    const auto inner = sha256_bytes(i_key_pad + data);
    std::string inner_data(reinterpret_cast<const char*>(inner.data()), inner.size());
    const auto outer = sha256_bytes(o_key_pad + inner_data);
    return hex_encode(outer.data(), outer.size());
}

std::string installer_key_proof(const std::string& installer_key,
                                const std::string& device_id,
                                const std::string& nonce,
                                long long timestamp_unix_seconds) {
    return hmac_sha256_hex(installer_key, device_id + "\n" + nonce + "\n" +
                                            std::to_string(timestamp_unix_seconds));
}

std::string derive_device_id(const std::string& platform_prefix,
                             const std::string& installer_key,
                             const std::string& hardware_material) {
    return platform_prefix + "-" +
           hmac_sha256_hex(installer_key, normalize_hardware_material(hardware_material));
}

std::string collect_linux_hardware_material_from_paths(const std::string& machine_id_path,
                                                       const std::string& dbus_machine_id_path,
                                                       const std::string& product_uuid_path,
                                                       const std::string& board_serial_path) {
    std::vector<std::string> parts;
    append_hardware_part(&parts, "machine_id", read_first_line(machine_id_path));
    append_hardware_part(&parts, "dbus_machine_id", read_first_line(dbus_machine_id_path));
    append_hardware_part(&parts, "dmi_product_uuid", read_first_line(product_uuid_path));
    append_hardware_part(&parts, "dmi_board_serial", read_first_line(board_serial_path));
    return join_hardware_parts(parts);
}

std::string parse_macos_ioplatform_uuid(const std::string& ioreg_output) {
    std::istringstream input(ioreg_output);
    std::string line;
    while (std::getline(input, line)) {
        if (line.find("IOPlatformUUID") == std::string::npos) {
            continue;
        }
        const auto equals = line.find('=');
        if (equals == std::string::npos) {
            continue;
        }
        const auto first_quote = line.find('"', equals);
        if (first_quote == std::string::npos) {
            continue;
        }
        const auto second_quote = line.find('"', first_quote + 1);
        if (second_quote == std::string::npos) {
            continue;
        }
        return trim(line.substr(first_quote + 1, second_quote - first_quote - 1));
    }
    return "";
}

std::string generate_nonce() {
    std::array<unsigned char, 16> bytes{};
    std::random_device rd;
    for (auto& b : bytes) {
        b = static_cast<unsigned char>(rd());
    }
    return hex_encode(bytes.data(), bytes.size());
}

std::string short_registration_code(const std::string& device_id) {
    const std::string h = sha256_hex(device_id);
    static const char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    std::string code;
    for (int i = 0; i < 8; ++i) {
        const std::string byte_hex = h.substr(static_cast<std::size_t>(i) * 2, 2);
        const int value = std::stoi(byte_hex, nullptr, 16);
        if (i == 4) code.push_back('-');
        code.push_back(alphabet[value % 32]);
    }
    return code;
}

std::string default_token_file() {
#ifdef _WIN32
    const std::string base = env_or_empty("PROGRAMDATA").empty()
                                 ? "."
                                 : env_or_empty("PROGRAMDATA") + "\\DeviceOps";
    return base + "\\device-agent-enrollment.json";
#elif defined(__ANDROID__)
    return "/data/local/tmp/device-agent-enrollment.json";
#else
    return "/var/lib/device-agent/enrollment.json";
#endif
}

void populate_enrollment_identity(terminal_agent::v1::EnrollRequest* request) {
    if (request == nullptr) {
        return;
    }
    request->set_hostname(hostname());
    request->set_platform(platform_prefix());
    request->set_os_arch(os_arch());
    request->set_agent_version(agent_version());
}

bool load_local_credential(const std::string& path, LocalCredential* out) {
    if (out == nullptr || path.empty()) return false;
    std::ifstream in(path);
    if (!in.is_open()) return false;
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    out->device_id = json_value(content, "device_id");
    out->token = json_value(content, "token");
    return !out->device_id.empty() && !out->token.empty();
}

bool save_local_credential(const std::string& path, const LocalCredential& cred,
                           std::string* error) {
    if (!ensure_parent_dir(path, error)) return false;
    std::ofstream out(path, std::ios::trunc);
    if (!out.is_open()) {
        if (error) *error = "open token file failed: " + path;
        return false;
    }
    out << "{\n"
        << "  \"device_id\": \"" << escape_json(cred.device_id) << "\",\n"
        << "  \"token\": \"" << escape_json(cred.token) << "\"\n"
        << "}\n";
    if (!out.good()) {
        if (error) *error = "write token file failed: " + path;
        return false;
    }
    return true;
}

bool write_enrollment_status(const std::string& path, const std::string& device_id,
                             const std::string& short_code,
                             const std::string& status,
                             const std::string& message,
                             std::string* error) {
    if (path.empty()) return true;
    if (!ensure_parent_dir(path, error)) return false;
    std::ofstream out(path, std::ios::trunc);
    if (!out.is_open()) {
        if (error) *error = "open enrollment status file failed: " + path;
        return false;
    }
    out << "{\n"
        << "  \"device_id\": \"" << escape_json(device_id) << "\",\n"
        << "  \"short_code\": \"" << escape_json(short_code) << "\",\n"
        << "  \"status\": \"" << escape_json(status) << "\",\n"
        << "  \"message\": \"" << escape_json(message) << "\"\n"
        << "}\n";
    return out.good();
}

std::string platform_prefix() {
#ifdef _WIN32
    return "win";
#elif defined(__ANDROID__)
    return "and";
#elif defined(__APPLE__)
    return "mac";
#elif defined(__linux__)
    return "linux";
#else
    return "dev";
#endif
}

std::string collect_hardware_material() {
#ifdef _WIN32
    std::vector<std::string> parts;
    append_hardware_part(&parts, "smbios_uuid", run_command_first_value("wmic csproduct get uuid"));
    append_hardware_part(&parts, "board_serial", run_command_first_value("wmic baseboard get serialnumber"));
    append_hardware_part(&parts, "machine_guid", run_command_first_value(
        "reg query HKLM\\SOFTWARE\\Microsoft\\Cryptography /v MachineGuid"));
    return join_hardware_parts(parts);
#elif defined(__ANDROID__)
    std::vector<std::string> parts;
    char serial[PROP_VALUE_MAX] = {};
    __system_property_get("ro.serialno", serial);
    append_hardware_part(&parts, "android_id", env_or_empty("DEVICE_AGENT_ANDROID_ID"));
    append_hardware_part(&parts, "serial", std::string(serial));
    append_hardware_part(&parts, "hostname", hostname());
    return join_hardware_parts(parts);
#elif defined(__APPLE__)
    const std::string uuid = parse_macos_ioplatform_uuid(
        run_command_output("ioreg -rd1 -c IOPlatformExpertDevice"));
    std::vector<std::string> parts;
    append_hardware_part(&parts, "ioplatform_uuid", uuid);
    return join_hardware_parts(parts);
#elif defined(__linux__)
    return collect_linux_hardware_material_from_paths(
        "/etc/machine-id",
        "/var/lib/dbus/machine-id",
        "/sys/class/dmi/id/product_uuid",
        "/sys/class/dmi/id/board_serial");
#else
    return "";
#endif
}

bool ensure_enrolled(Config* config) {
    if (config == nullptr) return false;
    const std::string token_file = resolved_token_file(*config);
    LocalCredential local;
    if (config->auth.token.empty() && load_local_credential(token_file, &local)) {
        config->auth.device_id = local.device_id;
        config->auth.token = local.token;
        LOG_INFO("Loaded enrollment credential from " + token_file);
    }
    if (!config->auth.device_id.empty() && !config->auth.token.empty()) {
        return true;
    }
    if (config->enrollment.installer_key_id.empty() || config->enrollment.installer_key.empty()) {
        LOG_ERROR("device token missing and enrollment installer key config is incomplete");
        return false;
    }
    const std::string hardware = collect_hardware_material();
    if (hardware.empty()) {
        LOG_ERROR("device token missing and hardware identity material is empty");
        return false;
    }
    const std::string device_id = config->auth.device_id.empty()
                                      ? derive_device_id(platform_prefix(), config->enrollment.installer_key, hardware)
                                      : config->auth.device_id;
    const std::string short_code = short_registration_code(device_id);
    const std::string status_file = resolved_status_file(*config, token_file);
    LOG_INFO("Enrollment required; short_code=" + short_code + " device_id=" + device_id);

    int backoff = std::max(1, config->enrollment.retry_base_seconds);
    const int max_backoff = std::max(backoff, config->enrollment.retry_max_seconds);
    while (true) {
        const EnrollOnceResult result = enroll_once(*config, device_id, hardware, short_code);
        std::string status_error;
        write_enrollment_status(status_file, device_id, short_code, result.status,
                                result.message, &status_error);
        if (!status_error.empty()) {
            LOG_WARN(status_error);
        }
        if (result.revoked) {
            config->auth.token.clear();
            std::remove(token_file.c_str());
        }
        if (result.active) {
            std::string save_error;
            if (!save_local_credential(token_file, result.credential, &save_error)) {
                LOG_ERROR(save_error);
                return false;
            }
            config->auth.device_id = result.credential.device_id;
            config->auth.token = result.credential.token;
            LOG_INFO("Enrollment active; credential saved to " + token_file);
            return true;
        }
        const int server_retry = result.retry_after_seconds > 0 ? result.retry_after_seconds : backoff;
        const int sleep_seconds = std::min(max_backoff, std::max(1, server_retry));
        LOG_WARN("Enrollment not active: status=" + result.status + " message=" + result.message +
                 "; retry in " + std::to_string(sleep_seconds) + "s");
        std::this_thread::sleep_for(std::chrono::seconds(sleep_seconds));
        backoff = std::min(max_backoff, backoff * 2);
    }
}

}  // namespace enrollment
}  // namespace device_agent
