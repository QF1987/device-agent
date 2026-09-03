<#
.SYNOPSIS
  打包 DeviceOps device-agent 装机套件 zip(供 D2 引导链接 file_id 用)。
.DESCRIPTION
  套件 = device-agent.exe + 其全部 DLL + install-device-agent.ps1 + install-device-agent.cmd,
  平铺在 zip 根(装机时 ps1 与 exe 必须同目录,才能把同目录 exe 当离线本地包 side-by-side 装)。
  本脚本只打包,不构建;device-agent.exe + DLL 来自你的 Windows 构建产物目录(-AgentDir)。
  会校验 DLL 是否存在 —— 缺 DLL 装到设备会 1053(找不到 libprotobuf.dll)起不来。
.PARAMETER AgentDir
  含 device-agent.exe + 其 DLL 的目录(构建产物目录)。必填。
.PARAMETER AgentVersion
  套件版本，必须与 device-agent.exe --version 完全一致；正式测试包不可省略。
.PARAMETER P2P
  ADR-20260831-01 B3：打 P2P-enabled 套件。要求 AgentDir 已含
  torrent-rasterbar.dll + libcrypto-3-x64.dll（DEVICE_AGENT_ENABLE_WINDOWS_P2P=ON
  构建产物），并显式拒绝 Debug CRT DLL；DEVICE_AGENT_MANIFEST.txt 记录
  windows_p2p=enabled。非 P2P 套件记录 windows_p2p=disabled。
.PARAMETER ScriptDir
  含 install-device-agent.ps1/.cmd 的目录,默认本脚本所在的 windows-bootstrap 目录。
.PARAMETER OutZip
  输出 zip 路径,默认当前目录 .\device-agent-kit.zip。
.PARAMETER VcRedistPath
  可选。Microsoft vc_redist.x64.exe 路径；提供后放入 prerequisites/，并写入 SHA256SUMS.txt。
.EXAMPLE
  .\make-kit.ps1 -AgentDir C:\build\device-agent\Release -AgentVersion 1.2.3 -VcRedistPath C:\deps\vc_redist.x64.exe -OutZip C:\Users\qf\Desktop\device-agent-kit.zip
.EXAMPLE
  P2P-enabled 套件（DEVICE_AGENT_ENABLE_WINDOWS_P2P=ON 构建产物）:
  .\make-kit.ps1 -AgentDir C:\build\device-agent\Release -AgentVersion 1.2.3 -P2P -OutZip C:\out\device-agent-kit-p2p.zip
#>
[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)][string]$AgentDir,
  [Parameter(Mandatory = $true)][string]$AgentVersion,
  [switch]$P2P,
  [string]$ScriptDir = $PSScriptRoot,
  [string]$OutZip = ".\device-agent-kit.zip",
  [string]$VcRedistPath
)
$ErrorActionPreference = 'Stop'

Import-Module (Join-Path $PSScriptRoot 'make-kit-lib.psm1') -Force

if (-not (Test-AgentVersionFormat -Version $AgentVersion)) {
  throw "AgentVersion 必须是不带 v 前缀的 SemVer: $AgentVersion"
}

$exe = Join-Path $AgentDir 'device-agent.exe'
if (-not (Test-Path $exe)) {
  throw "找不到 device-agent.exe: $exe(用 -AgentDir 指向你的构建产物目录)"
}
$versionOutput = (& $exe --version 2>&1 | Out-String).Trim()
if ($LASTEXITCODE -ne 0) {
  throw "无法读取 device-agent.exe 版本(exit=$LASTEXITCODE): $versionOutput"
}
$expectedVersionOutput = 'device-agent ' + $AgentVersion
if ($versionOutput -ne $expectedVersionOutput) {
  throw "版本不一致: 参数=$AgentVersion, binary='$versionOutput'"
}

if ($P2P) {
  # ADR-20260831-01 D7：P2P kit 的 loader 依赖闭包显式校验。
  $p2pMissing = Get-P2PClosureMissing -AgentDir $AgentDir
  if ($p2pMissing.Count -gt 0) {
    throw ("P2P kit 缺少依赖 DLL: " + ($p2pMissing -join ', ') +
           " —— 请确认 -AgentDir 是 DEVICE_AGENT_ENABLE_WINDOWS_P2P=ON 的构建产物目录。")
  }
}
# Release CRT 约束（P2P kit 强制；Debug CRT 混入装到设备会 1053）。
$debugCrt = Get-DebugCrtPresent -AgentDir $AgentDir
if ($P2P -and $debugCrt.Count -gt 0) {
  throw ("P2P kit 检测到 Debug CRT DLL(禁止混用): " + ($debugCrt -join ', ') +
         " —— 请用 Release 配置产物重新打包。")
}

$ps1 = Join-Path $ScriptDir 'install-device-agent.ps1'
$cmd = Join-Path $ScriptDir 'install-device-agent.cmd'
foreach ($f in @($ps1, $cmd)) {
  if (-not (Test-Path $f)) {
    throw "找不到装机脚本: $f(用 -ScriptDir 指向 windows-bootstrap 目录)"
  }
}

$stage = Join-Path $env:TEMP ('da-kit-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $stage | Out-Null
try {
  Copy-Item $exe $stage -Force
  $dlls = @(Get-ChildItem -Path $AgentDir -Filter '*.dll' -File)
  if ($dlls.Count -eq 0) {
    throw "AgentDir 里没有任何 *.dll($AgentDir)。device-agent.exe 依赖 DLL(libprotobuf/libssl/libcrypto/abseil/cares/re2/zlib 等),缺了装到设备会 1053 起不来 —— 请确认 -AgentDir 指向的是完整构建产物目录。"
  }
  $dlls | ForEach-Object { Copy-Item $_.FullName $stage -Force }
  Copy-Item $ps1 $stage -Force
  Copy-Item $cmd $stage -Force
  # 冒号参数后不能直接放 cast 表达式（会被当字符串）——先预计算布尔。
  $p2pEnabled = [bool]$P2P
  Set-Content -LiteralPath (Join-Path $stage 'DEVICE_AGENT_MANIFEST.txt') `
    -Value (New-KitManifestLines -Version $AgentVersion -P2P:$p2pEnabled) -Encoding ASCII

  if ($VcRedistPath) {
    if (-not (Test-Path -LiteralPath $VcRedistPath)) {
      throw "找不到 VC++ Runtime: $VcRedistPath"
    }
    $prereqDir = Join-Path $stage 'prerequisites'
    New-Item -ItemType Directory -Force -Path $prereqDir | Out-Null
    Copy-Item -LiteralPath $VcRedistPath -Destination (Join-Path $prereqDir 'vc_redist.x64.exe') -Force
  }

  $manifestLines = @()
  Get-ChildItem -LiteralPath $stage -File -Recurse | Sort-Object FullName | ForEach-Object {
    $relative = $_.FullName.Substring($stage.Length).TrimStart('\').Replace('\', '/')
    $hash = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    $manifestLines += ($hash + '  ' + $relative)
  }
  Set-Content -LiteralPath (Join-Path $stage 'SHA256SUMS.txt') -Value $manifestLines -Encoding ASCII

  $OutZip = [System.IO.Path]::GetFullPath($OutZip)
  if (Test-Path $OutZip) { Remove-Item $OutZip -Force }
  # -Path 指 stage\* → 文件平铺在 zip 根(exe 与 ps1 同层)。
  Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $OutZip -Force

  Write-Host ('套件已生成: ' + $OutZip + (' (windows_p2p=' + ($(if ($P2P) { 'enabled' } else { 'disabled' })) + ')'))
  Write-Host ('内容(' + (Get-ChildItem $stage -File -Recurse).Count + ' 个文件,exe + ' + $dlls.Count + ' dll + ps1 + cmd + version manifest + SHA256SUMS):')
  Get-ChildItem $stage -File -Recurse | Format-Table FullName, Length -AutoSize | Out-String | ForEach-Object { Write-Host $_ }
  Write-Host ('sha256: ' + (Get-FileHash $OutZip -Algorithm SHA256).Hash)
  Write-Host ''
  Write-Host '下一步:用 dashboard「上传套件」/ chat upload_package / curl 传它拿 file_id → 签 turnkey 引导链接。'
} finally {
  Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
}
