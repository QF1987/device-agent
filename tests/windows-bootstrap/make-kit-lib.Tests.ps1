<#
.SYNOPSIS
  make-kit-lib.psm1 离线确定性测试（ADR-20260831-01 B3 packaging Phase A）。
.DESCRIPTION
  纯本地：临时目录 + 假 DLL 文件名即可，无需真实二进制、无网络。
  兼容 Windows PowerShell 5.1 与 pwsh 7+。exit 0 = 全部通过。
  运行：pwsh -NoProfile -File tests\windows-bootstrap\make-kit-lib.Tests.ps1
#>

$ErrorActionPreference = 'Stop'

$libPath = Join-Path $PSScriptRoot '..\..\scripts\windows-bootstrap\make-kit-lib.psm1'
Import-Module $libPath -Force

$script:failures = 0

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if ($Condition) {
        Write-Host ("  PASS " + $Message)
    } else {
        Write-Host ("  FAIL " + $Message)
        $script:failures++
    }
}

function New-FixtureAgentDir {
    param([string]$Name, [string[]]$Files)
    $dir = Join-Path ([System.IO.Path]::GetTempPath()) ('kit-lib-test-' + $Name + '-' + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    foreach ($f in $Files) {
        Set-Content -LiteralPath (Join-Path $dir $f) -Value 'fixture' -Encoding ASCII
    }
    return $dir
}

Write-Host '== Test-AgentVersionFormat =='
Assert-True (Test-AgentVersionFormat -Version '1.2.3') '1.2.3 valid'
Assert-True (Test-AgentVersionFormat -Version '0.1.0-win-p2p-b3.20260901') 'prerelease valid'
Assert-True (Test-AgentVersionFormat -Version '1.2.3-rc.1') 'rc suffix valid'
Assert-True (-not (Test-AgentVersionFormat -Version 'v1.2.3')) 'leading v rejected'
Assert-True (-not (Test-AgentVersionFormat -Version '1.2')) 'two segments rejected'
Assert-True (-not (Test-AgentVersionFormat -Version '')) 'empty rejected'
Assert-True (-not (Test-AgentVersionFormat -Version 'abc')) 'non-semver rejected'

Write-Host '== Get-P2PClosureMissing =='
$full = New-FixtureAgentDir -Name 'full' -Files @('device-agent.exe', 'torrent-rasterbar.dll', 'libcrypto-3-x64.dll', 'libprotobuf.dll')
Assert-True ((Get-P2PClosureMissing -AgentDir $full).Count -eq 0) 'closure satisfied with both DLLs'

$noTorrent = New-FixtureAgentDir -Name 'notorrent' -Files @('device-agent.exe', 'libcrypto-3-x64.dll')
$missing = Get-P2PClosureMissing -AgentDir $noTorrent
Assert-True ($missing.Count -eq 1 -and $missing[0] -eq 'torrent-rasterbar.dll') 'missing torrent-rasterbar detected'

$noCrypto = New-FixtureAgentDir -Name 'nocrypto' -Files @('device-agent.exe', 'torrent-rasterbar.dll')
$missing = Get-P2PClosureMissing -AgentDir $noCrypto
Assert-True ($missing.Count -eq 1 -and $missing[0] -eq 'libcrypto-3-x64.dll') 'missing libcrypto detected'

$empty = New-FixtureAgentDir -Name 'empty' -Files @()
$missing = Get-P2PClosureMissing -AgentDir $empty
Assert-True ($missing.Count -eq 2) 'empty dir missing both'

Write-Host '== Get-DebugCrtPresent =='
$clean = New-FixtureAgentDir -Name 'clean' -Files @('device-agent.exe', 'libcrypto-3-x64.dll')
Assert-True ((Get-DebugCrtPresent -AgentDir $clean).Count -eq 0) 'no Debug CRT = clean'

foreach ($crt in @('msvcp140d.dll', 'vcruntime140d.dll', 'vcruntime140_1d.dll', 'ucrtbased.dll')) {
    $dirty = New-FixtureAgentDir -Name ('dirty-' + $crt) -Files @($crt)
    $found = Get-DebugCrtPresent -AgentDir $dirty
    Assert-True ($found.Count -eq 1 -and $found[0] -eq $crt) ("Debug CRT detected: " + $crt)
}
$mixed = New-FixtureAgentDir -Name 'mixed' -Files @('msvcp140.dll', 'vcruntime140.dll', 'msvcp140d.dll')
Assert-True ((Get-DebugCrtPresent -AgentDir $mixed).Count -eq 1) 'Release CRT not flagged, Debug CRT flagged'

Write-Host '== New-KitManifestLines =='
$lines = New-KitManifestLines -Version '1.2.3' -P2P:$true
Assert-True ($lines.Count -eq 3) 'manifest has 3 lines'
Assert-True ($lines[0] -eq 'format=1') 'manifest format=1'
Assert-True ($lines[1] -eq 'agent_version=1.2.3') 'manifest agent_version'
Assert-True ($lines[2] -eq 'windows_p2p=enabled') 'manifest windows_p2p=enabled'

$lines = New-KitManifestLines -Version '1.2.3' -P2P:$false
Assert-True ($lines[2] -eq 'windows_p2p=disabled') 'manifest windows_p2p=disabled'

Write-Host ''
if ($script:failures -gt 0) {
    Write-Host ("FAILED: " + $script:failures + " case(s)")
    exit 1
}
Write-Host 'make-kit-lib.Tests.ps1 PASS'
exit 0
