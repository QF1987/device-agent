// ============================================================
// executor/installer_classifier.cc - 安装包家族分类实现（Slice E1）
// ============================================================

#include "executor/installer_classifier.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace device_agent {

namespace {

// 安装包可能很大（目标包 451 MB）。标记只可能出现在头部（PE 头/段名/Inno
// 装载器数据）或尾部（NSIS 追加数据块），故只扫描首尾各一段，避免整文件读盘。
constexpr std::size_t kScanWindowBytes = 4UL * 1024UL * 1024UL;

bool ends_with_ci(const std::string& s, const std::string& suffix) {
    if (s.size() < suffix.size()) {
        return false;
    }
    return std::equal(suffix.rbegin(), suffix.rend(), s.rbegin(),
                      [](char a, char b) {
                          return std::tolower(static_cast<unsigned char>(a)) ==
                                 std::tolower(static_cast<unsigned char>(b));
                      });
}

// 在 buffer 中查找 ASCII 字面量 marker。
bool buffer_contains(const std::vector<char>& buf, const std::string& marker) {
    if (marker.empty() || buf.size() < marker.size()) {
        return false;
    }
    return std::search(buf.begin(), buf.end(), marker.begin(), marker.end()) !=
           buf.end();
}

}  // namespace

InstallerFamily classify_installer(const std::string& packagePath) {
    std::ifstream f(packagePath, std::ios::binary);
    if (!f.is_open()) {
        return InstallerFamily::Unknown;
    }

    // 1) 文件头魔数。
    unsigned char header[8] = {0};
    f.read(reinterpret_cast<char*>(header), sizeof(header));
    const std::streamsize header_read = f.gcount();

    // OLE 复合文档魔数（MSI 即 OLE 容器）：D0 CF 11 E0 A1 B1 1A E1。
    static const unsigned char kOle[8] = {0xD0, 0xCF, 0x11, 0xE0,
                                          0xA1, 0xB1, 0x1A, 0xE1};
    if (header_read >= 4 && std::equal(header, header + 4, kOle)) {
        return InstallerFamily::MSI;
    }
    if (ends_with_ci(packagePath, ".msi")) {
        return InstallerFamily::MSI;
    }

    // 非 PE（无 MZ 头）则无从扫标记。
    const bool is_pe = header_read >= 2 && header[0] == 'M' && header[1] == 'Z';
    if (!is_pe) {
        return InstallerFamily::Unknown;
    }

    // 2) 扫首尾各一段查 PE ASCII 标记。
    f.clear();
    f.seekg(0, std::ios::end);
    const std::streamoff size = f.tellg();
    if (size <= 0) {
        return InstallerFamily::Unknown;
    }

    std::vector<char> buf;

    // 头部窗口。
    {
        const std::size_t head_len = static_cast<std::size_t>(
            std::min<std::streamoff>(size, static_cast<std::streamoff>(kScanWindowBytes)));
        buf.resize(head_len);
        f.clear();
        f.seekg(0, std::ios::beg);
        f.read(buf.data(), static_cast<std::streamsize>(head_len));
        buf.resize(static_cast<std::size_t>(f.gcount()));

        if (buffer_contains(buf, ".wixburn")) return InstallerFamily::WiXBurn;
        if (buffer_contains(buf, "Inno Setup Setup Data")) return InstallerFamily::InnoSetup;
        if (buffer_contains(buf, "Nullsoft")) return InstallerFamily::NSIS;
    }

    // 尾部窗口（与头部不重叠时才单独再读）。NSIS 数据块常追加在尾部。
    if (static_cast<std::streamoff>(kScanWindowBytes) < size) {
        const std::streamoff tail_start = size - static_cast<std::streamoff>(kScanWindowBytes);
        buf.assign(kScanWindowBytes, '\0');
        f.clear();
        f.seekg(tail_start, std::ios::beg);
        f.read(buf.data(), static_cast<std::streamsize>(kScanWindowBytes));
        buf.resize(static_cast<std::size_t>(f.gcount()));

        if (buffer_contains(buf, ".wixburn")) return InstallerFamily::WiXBurn;
        if (buffer_contains(buf, "Inno Setup Setup Data")) return InstallerFamily::InnoSetup;
        if (buffer_contains(buf, "Nullsoft")) return InstallerFamily::NSIS;
    }

    return InstallerFamily::Unknown;
}

std::string default_silent_args(InstallerFamily family) {
    switch (family) {
        case InstallerFamily::InnoSetup:
            return "/VERYSILENT /SUPPRESSMSGBOXES /NORESTART";
        case InstallerFamily::WiXBurn:
            return "/quiet /norestart";
        case InstallerFamily::NSIS:
            return "/S";
        case InstallerFamily::MSI:
        case InstallerFamily::Unknown:
        default:
            // MSI 需经 msiexec、不能 CreateProcess 直跑；Unknown 无可靠默认。
            // 此时不补默认开关，交由 operator 显式给 install_args。
            return std::string();
    }
}

std::string default_success_codes(InstallerFamily family) {
    switch (family) {
        case InstallerFamily::InnoSetup:
        case InstallerFamily::WiXBurn:
            return "0,3010";
        case InstallerFamily::MSI:
            return "0,3010,1641";
        case InstallerFamily::NSIS:
        case InstallerFamily::Unknown:
        default:
            return "0";
    }
}

const char* installer_family_name(InstallerFamily family) {
    switch (family) {
        case InstallerFamily::MSI:       return "MSI";
        case InstallerFamily::InnoSetup: return "InnoSetup";
        case InstallerFamily::WiXBurn:   return "WiXBurn";
        case InstallerFamily::NSIS:      return "NSIS";
        case InstallerFamily::Unknown:
        default:                         return "Unknown";
    }
}

}  // namespace device_agent
