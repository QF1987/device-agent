// ============================================================
// executor/installer_classifier_test.cc - 安装包分类单测（Slice E1）
// ============================================================
// 用小体积 fixture 文件覆盖各家族识别 + 默认开关映射，Mac/Linux 可跑。

#include "executor/installer_classifier.h"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>

namespace {

bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        return false;
    }
    return true;
}

// 写一个 fixture 文件：header 原始字节 + 追加 payload 文本。
std::string write_fixture(const std::string& name,
                          const std::string& header_bytes,
                          const std::string& payload) {
    const std::string path = std::string("/tmp/device-agent-classifier-") + name;
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(header_bytes.data(), static_cast<std::streamsize>(header_bytes.size()));
    f.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    f.close();
    return path;
}

}  // namespace

int main() {
    using device_agent::InstallerFamily;
    bool ok = true;

    const std::string mz = "MZ";
    const std::string ole = "\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1";

    // MSI：OLE 复合文档魔数。
    {
        const std::string p = write_fixture("msi.bin", ole, std::string(64, 'x'));
        const auto fam = device_agent::classify_installer(p);
        ok &= expect(fam == InstallerFamily::MSI, "OLE magic should classify as MSI");
        std::remove(p.c_str());
    }

    // MSI：.msi 扩展名（非 OLE 头也认）。
    {
        const std::string p = write_fixture("pkg.msi", mz, std::string(64, 'x'));
        const auto fam = device_agent::classify_installer(p);
        ok &= expect(fam == InstallerFamily::MSI, ".msi extension should classify as MSI");
        std::remove(p.c_str());
    }

    // Inno Setup：PE + `Inno Setup Setup Data` 标记。
    {
        const std::string p = write_fixture(
            "inno.exe", mz, std::string(4096, '\0') + "Inno Setup Setup Data (5.5.0)");
        const auto fam = device_agent::classify_installer(p);
        ok &= expect(fam == InstallerFamily::InnoSetup, "Inno marker should classify as InnoSetup");
        ok &= expect(device_agent::default_silent_args(fam) ==
                         "/VERYSILENT /SUPPRESSMSGBOXES /NORESTART",
                     "Inno default silent args");
        ok &= expect(device_agent::default_success_codes(fam) == "0,3010",
                     "Inno default success codes");
        std::remove(p.c_str());
    }

    // WiX Burn：PE + `.wixburn` 段名。
    {
        const std::string p = write_fixture("wix.exe", mz, std::string(256, '\0') + ".wixburn");
        const auto fam = device_agent::classify_installer(p);
        ok &= expect(fam == InstallerFamily::WiXBurn, ".wixburn marker should classify as WiXBurn");
        std::remove(p.c_str());
    }

    // NSIS：PE + `Nullsoft` 标记。
    {
        const std::string p = write_fixture("nsis.exe", mz, std::string(256, '\0') + "Nullsoft Install System");
        const auto fam = device_agent::classify_installer(p);
        ok &= expect(fam == InstallerFamily::NSIS, "Nullsoft marker should classify as NSIS");
        ok &= expect(device_agent::default_silent_args(fam) == "/S", "NSIS default silent args");
        std::remove(p.c_str());
    }

    // Unknown：PE 无任何已知标记。
    {
        const std::string p = write_fixture("unknown.exe", mz, std::string(256, 'z'));
        const auto fam = device_agent::classify_installer(p);
        ok &= expect(fam == InstallerFamily::Unknown, "no marker should classify as Unknown");
        ok &= expect(device_agent::default_silent_args(fam).empty(),
                     "Unknown should have no default silent args");
        ok &= expect(device_agent::default_success_codes(fam) == "0",
                     "Unknown default success codes fall back to 0");
        std::remove(p.c_str());
    }

    // 打不开的文件 → Unknown（不崩）。
    {
        const auto fam = device_agent::classify_installer("/tmp/does-not-exist-xyz.bin");
        ok &= expect(fam == InstallerFamily::Unknown, "missing file should classify as Unknown");
    }

    if (ok) {
        std::cout << "installer_classifier_test: ALL PASS\n";
    }
    return ok ? 0 : 1;
}
