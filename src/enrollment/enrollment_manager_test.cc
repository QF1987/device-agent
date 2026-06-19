#include "enrollment/enrollment_manager.h"

#include <cassert>
#include <cstdio>
#include <fstream>
#include <string>

int main() {
    using namespace device_agent::enrollment;

    assert(sha256_hex("abc") ==
           "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    assert(hmac_sha256_hex("key", "The quick brown fox jumps over the lazy dog") ==
           "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8");
    assert(installer_key_proof("secret", "win-abc", "nonce", 123) ==
           hmac_sha256_hex("secret", "win-abc\nnonce\n123"));

    const std::string material = normalize_hardware_material(" SMBIOS=ABC \n GUID=DEF ");
    const std::string device_id = derive_device_id("win", "secret", material);
    assert(device_id.rfind("win-", 0) == 0);
    assert(device_id.size() == 68);

    const std::string code = short_registration_code(device_id);
    assert(code.size() == 9);
    assert(code[4] == '-');

    const std::string path = "/tmp/device-agent-enrollment-test.json";
    LocalCredential cred;
    cred.device_id = device_id;
    cred.token = "tok";
    std::string err;
    assert(save_local_credential(path, cred, &err));
    LocalCredential loaded;
    assert(load_local_credential(path, &loaded));
    assert(loaded.device_id == cred.device_id);
    assert(loaded.token == cred.token);
    std::remove(path.c_str());

    return 0;
}
