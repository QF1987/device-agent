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
.PARAMETER ScriptDir
  含 install-device-agent.ps1/.cmd 的目录,默认本脚本所在的 windows-bootstrap 目录。
.PARAMETER OutZip
  输出 zip 路径,默认当前目录 .\device-agent-kit.zip。
.PARAMETER VcRedistPath
  可选。Microsoft vc_redist.x64.exe 路径；提供后放入 prerequisites/，并写入 SHA256SUMS.txt。
.EXAMPLE
  .\make-kit.ps1 -AgentDir C:\build\device-agent\Release -VcRedistPath C:\deps\vc_redist.x64.exe -OutZip C:\Users\qf\Desktop\device-agent-kit.zip
#>
[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)][string]$AgentDir,
  [string]$ScriptDir = $PSScriptRoot,
  [string]$OutZip = ".\device-agent-kit.zip",
  [string]$VcRedistPath
)
$ErrorActionPreference = 'Stop'

$exe = Join-Path $AgentDir 'device-agent.exe'
if (-not (Test-Path $exe)) {
  throw "找不到 device-agent.exe: $exe(用 -AgentDir 指向你的构建产物目录)"
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

  Write-Host ('套件已生成: ' + $OutZip)
  Write-Host ('内容(' + (Get-ChildItem $stage -File -Recurse).Count + ' 个文件,exe + ' + $dlls.Count + ' dll + ps1 + cmd + SHA256SUMS):')
  Get-ChildItem $stage -File -Recurse | Format-Table FullName, Length -AutoSize | Out-String | ForEach-Object { Write-Host $_ }
  Write-Host ('sha256: ' + (Get-FileHash $OutZip -Algorithm SHA256).Hash)
  Write-Host ''
  Write-Host '下一步:用 dashboard「上传套件」/ chat upload_package / curl 传它拿 file_id → 签 turnkey 引导链接。'
} finally {
  Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
}
