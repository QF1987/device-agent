// ============================================================
// executor/installer_classifier.h - 安装包家族分类（Slice E1）
// ============================================================
// 通过读文件头魔数 + 扫描 PE 二进制 ASCII 标记，判定本地安装包属于
// 哪个安装器家族（MSI / Inno Setup / WiX Burn / NSIS / unknown）。
//
// 用途：当 operator 未在 release 中显式给出 install_args 时，按家族补一组
//       默认静默开关 + 默认成功退出码。**不**用于判断「能否静默」——
//       那由试静默 + 软超时（windows_executor::installPackage）裁定。
//
// 设计：纯文件读取逻辑，不依赖任何平台 API，可在所有平台编译与单测。
// ============================================================

#pragma once

#include <string>

namespace device_agent {

// 安装器家族。
enum class InstallerFamily {
    Unknown = 0,
    MSI,
    InnoSetup,
    WiXBurn,
    NSIS,
};

// classify_installer：读安装包文件头魔数 + 扫 PE ASCII 标记判定家族。
//   - OLE 复合文档魔数（D0 CF 11 E0）或 .msi 扩展名 → MSI
//   - PE（MZ）则扫二进制标记：`.wixburn` → WiXBurn / `Inno Setup Setup Data`
//     → InnoSetup / `Nullsoft` → NSIS / 否则 Unknown
//   - 打不开文件或无法识别 → Unknown
InstallerFamily classify_installer(const std::string& packagePath);

// default_silent_args：家族对应的默认静默参数（operator 未给 install_args 时用）。
//   Unknown / MSI 返回空串（无可靠的 CreateProcess 直跑默认）。
std::string default_silent_args(InstallerFamily family);

// default_success_codes：家族对应的默认成功退出码（逗号分隔）。
std::string default_success_codes(InstallerFamily family);

// installer_family_name：家族枚举的可读名称（日志用）。
const char* installer_family_name(InstallerFamily family);

}  // namespace device_agent
