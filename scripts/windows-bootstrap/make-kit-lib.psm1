<#
.SYNOPSIS
  make-kit.ps1 的纯校验/清单构造函数库（无副作用，可被离线测试 dot-source）。
.DESCRIPTION
  ADR-20260831-01 B3 packaging：P2P-enabled kit 的显式闭包与 CRT 约束、
  版本格式门、DEVICE_AGENT_MANIFEST.txt 内容。全部为纯函数，便于
  tests/windows-bootstrap/make-kit-lib.Tests.ps1 离线确定性验证。
#>

# AgentVersion 必须是不带 v 前缀的 SemVer（与 CMake -DDEVICE_AGENT_VERSION 同规）。
function Test-AgentVersionFormat {
    param([string]$Version)
    if ([string]::IsNullOrEmpty($Version)) { return $false }
    if ($Version.StartsWith('v')) { return $false }
    return $Version -match '^[0-9]+\.[0-9]+\.[0-9]+([.-][0-9A-Za-z.-]+)?$'
}

# P2P-enabled kit 的显式闭包要求（ADR-20260831-01 D7）：
# torrent-rasterbar.dll（libtorrent）+ libcrypto-3-x64.dll（OpenSSL，传递依赖）。
# 返回缺失文件名数组；空数组 = closure 满足。
function Get-P2PClosureMissing {
    param([string]$AgentDir)
    $missing = @()
    foreach ($name in @('torrent-rasterbar.dll', 'libcrypto-3-x64.dll')) {
        if (-not (Test-Path -LiteralPath (Join-Path $AgentDir $name) -PathType Leaf)) {
            $missing += $name
        }
    }
    # 前置逗号防止单元素数组被管线展开成标量（调用方 $x[0] 会取到首字符）。
    return ,$missing
}

# Release CRT 约束：kit 不得混入 Debug CRT（*d.dll）——装到设备会因
# Debug Runtime 缺失而 1053。返回发现的 Debug CRT 文件名数组；空 = 干净。
function Get-DebugCrtPresent {
    param([string]$AgentDir)
    $found = @()
    foreach ($name in @('msvcp140d.dll', 'vcruntime140d.dll', 'vcruntime140_1d.dll', 'ucrtbased.dll')) {
        if (Test-Path -LiteralPath (Join-Path $AgentDir $name) -PathType Leaf) {
            $found += $name
        }
    }
    return ,$found
}

# DEVICE_AGENT_MANIFEST.txt 内容（ADR-20260831-01 D7 / task §3.5）：
# format=1、agent_version（与 --version 逐字一致）、windows_p2p=enabled|disabled。
function New-KitManifestLines {
    param(
        [string]$Version,
        [bool]$P2P
    )
    $p2pValue = 'disabled'
    if ($P2P) { $p2pValue = 'enabled' }
    return @('format=1', ('agent_version=' + $Version), ('windows_p2p=' + $p2pValue))
}

Export-ModuleMember -Function Test-AgentVersionFormat, Get-P2PClosureMissing, Get-DebugCrtPresent, New-KitManifestLines
