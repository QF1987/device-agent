#pragma once

#include <string>

#include "config/config.h"

namespace device_agent {
namespace enrollment {

struct LocalCredential {
    std::string device_id;
    std::string token;
};

struct EnrollOnceResult {
    bool active = false;
    bool retryable = false;
    bool revoked = false;
    int retry_after_seconds = 0;
    std::string status;
    std::string message;
    LocalCredential credential;
};

std::string normalize_hardware_material(const std::string& raw);
std::string sha256_hex(const std::string& data);
std::string hmac_sha256_hex(const std::string& key, const std::string& data);
std::string installer_key_proof(const std::string& installer_key,
                                const std::string& device_id,
                                const std::string& nonce,
                                long long timestamp_unix_seconds);
std::string derive_device_id(const std::string& platform_prefix,
                             const std::string& installer_key,
                             const std::string& hardware_material);
std::string generate_nonce();
std::string short_registration_code(const std::string& device_id);
std::string default_token_file();

bool load_local_credential(const std::string& path, LocalCredential* out);
bool save_local_credential(const std::string& path, const LocalCredential& cred,
                           std::string* error);
bool write_enrollment_status(const std::string& path, const std::string& device_id,
                             const std::string& short_code,
                             const std::string& status,
                             const std::string& message,
                             std::string* error);

std::string platform_prefix();
std::string collect_hardware_material();
bool ensure_enrolled(Config* config);

}  // namespace enrollment
}  // namespace device_agent
