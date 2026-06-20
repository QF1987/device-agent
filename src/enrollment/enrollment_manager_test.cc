#include "enrollment/enrollment_manager.h"

#include <cassert>
#include <cstdio>
#include <filesystem>
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

    const std::string linux_dir = "/tmp/device-agent-enrollment-linux-test";
    std::filesystem::create_directories(linux_dir);
    {
        std::ofstream(linux_dir + "/machine-id") << "  MID-ABC  \n";
        std::ofstream(linux_dir + "/dbus-machine-id") << "DBUS-DEF\n";
        std::ofstream(linux_dir + "/product_uuid") << "DMI-UUID\n";
        std::ofstream(linux_dir + "/board_serial") << "BOARD-SERIAL\n";
    }
    const std::string linux_material = collect_linux_hardware_material_from_paths(
        linux_dir + "/machine-id",
        linux_dir + "/dbus-machine-id",
        linux_dir + "/product_uuid",
        linux_dir + "/board_serial");
    assert(linux_material.find("machine_id=mid-abc") != std::string::npos);
    assert(linux_material.find("dbus_machine_id=dbus-def") != std::string::npos);
    assert(linux_material.find("dmi_product_uuid=dmi-uuid") != std::string::npos);
    assert(linux_material.find("dmi_board_serial=board-serial") != std::string::npos);
    const std::string linux_id = derive_device_id("linux", "secret", linux_material);
    assert(linux_id.rfind("linux-", 0) == 0);
    assert(linux_id.size() == 70);

    const std::string linux_no_dmi = collect_linux_hardware_material_from_paths(
        linux_dir + "/machine-id",
        linux_dir + "/dbus-machine-id",
        linux_dir + "/missing-product-uuid",
        linux_dir + "/missing-board-serial");
    assert(linux_no_dmi.find("machine_id=mid-abc") != std::string::npos);
    assert(linux_no_dmi.find("dmi_product_uuid") == std::string::npos);
    assert(derive_device_id("linux", "secret", linux_no_dmi) ==
           derive_device_id("linux", "secret", linux_no_dmi));
    std::filesystem::remove_all(linux_dir);

    const std::string mac_uuid = parse_macos_ioplatform_uuid(
        "    | |   \"IOPlatformUUID\" = \"AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE\"\n");
    assert(mac_uuid == "AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE");
    const std::string mac_material = normalize_hardware_material("ioplatform_uuid=" + mac_uuid);
    const std::string mac_id = derive_device_id("mac", "secret", mac_material);
    assert(mac_id.rfind("mac-", 0) == 0);
    assert(mac_id.size() == 68);
    assert(parse_macos_ioplatform_uuid("no uuid here").empty());

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
