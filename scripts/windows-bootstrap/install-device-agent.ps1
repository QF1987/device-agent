<#
DeviceOps device-agent Windows bootstrap installer.

Entry point for operators is install-device-agent.cmd, not this file. The cmd
wrapper self-elevates through UAC and runs this script with ExecutionPolicy
Bypass. The script is intentionally dependency-light for Win7 compatibility.

Exit codes:
  0 success
  2 not administrator
  3 missing required parameters
  4 package download or source not found
  5 package checksum mismatch
  6 package extraction failed
  7 service install/start failed
  8 prerequisite install failed
  1 other error
#>
[CmdletBinding()]
param(
  [string]$AnswerFile,
  [string]$ServerHost,
  [int]$ServerPort = 0,
  [string]$UseTls,
  [string]$DeviceId,
  [string]$Token,
  [string]$AgentUrl,
  [string]$AgentSha256,
  [string]$InstallDir,
  [string]$ServiceName,
  [string]$LogDir,
  [string]$VcRedistUrl,
  [string]$VcRedistSha256,
  [string]$UcrtUrl,
  [string]$UcrtSha256,
  [string]$BootstrapReportUrl,
  [switch]$Uninstall
)

$ErrorActionPreference = 'Stop'
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$DefaultInstallDir = Join-Path $env:ProgramFiles 'DeviceOps\device-agent'
$BootstrapRoot = Join-Path $env:ProgramData 'DeviceOps\bootstrap'
if (-not (Test-Path $BootstrapRoot)) {
  New-Item -ItemType Directory -Force -Path $BootstrapRoot | Out-Null
}
$BootstrapLog = Join-Path $BootstrapRoot 'install-device-agent.log'

function Write-Log([string]$Message) {
  $line = ('{0} {1}' -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $Message)
  Write-Host $line
  Add-Content -LiteralPath $BootstrapLog -Value $line -Encoding UTF8
}

function Fail([int]$Code, [string]$Message) {
  Write-Log ('ERROR: ' + $Message)
  exit $Code
}

function Test-IsAdmin {
  $id = [Security.Principal.WindowsIdentity]::GetCurrent()
  $principal = New-Object Security.Principal.WindowsPrincipal($id)
  return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Read-AnswerFile([string]$Path) {
  $out = @{}
  foreach ($line in Get-Content -LiteralPath $Path) {
    $trimmed = $line.Trim()
    if ($trimmed -eq '' -or $trimmed.StartsWith('#')) { continue }
    $parts = $trimmed.Split('=', 2)
    if ($parts.Count -eq 2) {
      $out[$parts[0].Trim().ToLowerInvariant()] = $parts[1].Trim()
    }
  }
  return $out
}

function Pick-Value([hashtable]$Answer, [string[]]$Keys, [string]$Current, [string]$DefaultValue) {
  if ($Current) { return $Current }
  foreach ($key in $Keys) {
    if ($Answer.ContainsKey($key) -and $Answer[$key]) { return $Answer[$key] }
  }
  return $DefaultValue
}

function Pick-Int([hashtable]$Answer, [string[]]$Keys, [int]$Current, [int]$DefaultValue) {
  if ($Current -gt 0) { return $Current }
  foreach ($key in $Keys) {
    if ($Answer.ContainsKey($key) -and $Answer[$key]) {
      $parsed = 0
      if ([int]::TryParse($Answer[$key], [ref]$parsed) -and $parsed -gt 0) {
        return $parsed
      }
    }
  }
  return $DefaultValue
}

function To-BoolString([string]$Value, [string]$DefaultValue) {
  if (-not $Value) { return $DefaultValue }
  $v = $Value.Trim().ToLowerInvariant()
  if ($v -eq '1' -or $v -eq 'true' -or $v -eq 'yes') { return 'true' }
  if ($v -eq '0' -or $v -eq 'false' -or $v -eq 'no') { return 'false' }
  return $DefaultValue
}

function ConvertTo-JsonString([string]$Value) {
  if ($null -eq $Value) { $Value = '' }
  return $Value.Replace('\', '\\').Replace('"', '\"')
}

function Get-OSInfo {
  $os = Get-WmiObject -Class Win32_OperatingSystem
  $cpu = Get-WmiObject -Class Win32_Processor | Select-Object -First 1
  return @{
    Caption = [string]$os.Caption
    Version = [version]$os.Version
    OSArchitecture = [string]$os.OSArchitecture
    AddressWidth = [int]$cpu.AddressWidth
    IsWin7 = ([version]$os.Version).Major -eq 6 -and ([version]$os.Version).Minor -eq 1
  }
}

function Enable-Tls12Client {
  Write-Log 'Ensuring TLS 1.2 client settings for WinHTTP, Schannel, and .NET'
  $paths = @(
    'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Internet Settings\WinHttp',
    'HKLM:\SOFTWARE\Wow6432Node\Microsoft\Windows\CurrentVersion\Internet Settings\WinHttp'
  )
  foreach ($path in $paths) {
    if (-not (Test-Path $path)) { New-Item -Path $path -Force | Out-Null }
    New-ItemProperty -Path $path -Name DefaultSecureProtocols -Value 0x800 -PropertyType DWord -Force | Out-Null
  }

  $schannel = 'HKLM:\SYSTEM\CurrentControlSet\Control\SecurityProviders\SCHANNEL\Protocols\TLS 1.2\Client'
  if (-not (Test-Path $schannel)) { New-Item -Path $schannel -Force | Out-Null }
  New-ItemProperty -Path $schannel -Name DisabledByDefault -Value 0 -PropertyType DWord -Force | Out-Null
  New-ItemProperty -Path $schannel -Name Enabled -Value 1 -PropertyType DWord -Force | Out-Null

  foreach ($path in @(
      'HKLM:\SOFTWARE\Microsoft\.NETFramework\v4.0.30319',
      'HKLM:\SOFTWARE\Wow6432Node\Microsoft\.NETFramework\v4.0.30319')) {
    if (-not (Test-Path $path)) { New-Item -Path $path -Force | Out-Null }
    New-ItemProperty -Path $path -Name SchUseStrongCrypto -Value 1 -PropertyType DWord -Force | Out-Null
    New-ItemProperty -Path $path -Name SystemDefaultTlsVersions -Value 1 -PropertyType DWord -Force | Out-Null
  }
}

function Enable-ProcessTls12 {
  try {
    [Net.ServicePointManager]::SecurityProtocol =
      [Net.ServicePointManager]::SecurityProtocol -bor 3072
  } catch {
    Write-Log ('WARN: unable to enable process TLS1.2: ' + $_.Exception.Message)
  }
}

function Get-FileSha256([string]$Path) {
  $sha = New-Object System.Security.Cryptography.SHA256Managed
  $stream = [System.IO.File]::OpenRead($Path)
  try {
    $bytes = $sha.ComputeHash($stream)
    return ([BitConverter]::ToString($bytes)).Replace('-', '').ToLowerInvariant()
  } finally {
    $stream.Close()
    $sha.Dispose()
  }
}

function Assert-Sha256([string]$Path, [string]$Expected, [int]$ExitCode) {
  if (-not $Expected) { return }
  $actual = Get-FileSha256 $Path
  if ($actual -ne $Expected.ToLowerInvariant()) {
    Fail $ExitCode ('sha256 mismatch for {0}: expected={1} actual={2}' -f $Path, $Expected, $actual)
  }
  Write-Log ('sha256 verified: ' + $Path)
}

function Download-File([string]$Url, [string]$Path) {
  Write-Log ('Downloading ' + $Url)
  $wc = New-Object System.Net.WebClient
  try {
    $wc.Headers.Add('User-Agent', 'DeviceOps-device-agent-bootstrap/1.0')
    $wc.DownloadFile($Url, $Path)
  } finally {
    $wc.Dispose()
  }
}

function Invoke-CheckedProcess([string]$FilePath, [string[]]$Arguments, [int]$ExitCode, [string]$Label) {
  Write-Log ('Running ' + $Label)
  $p = Start-Process -FilePath $FilePath -ArgumentList $Arguments -Wait -PassThru -WindowStyle Hidden
  if ($p.ExitCode -ne 0) {
    Fail $ExitCode ('{0} failed with exit code {1}' -f $Label, $p.ExitCode)
  }
}

function Install-Prerequisites([hashtable]$OSInfo) {
  Enable-Tls12Client
  Enable-ProcessTls12

  if ($VcRedistUrl) {
    $vcPath = Join-Path $BootstrapRoot 'vc_redist.x64.exe'
    Download-File $VcRedistUrl $vcPath
    Assert-Sha256 $vcPath $VcRedistSha256 8
    Invoke-CheckedProcess $vcPath @('/install', '/quiet', '/norestart') 8 'VC++ redistributable'
  } else {
    Write-Log 'WARN: vc_redist_url not provided; assuming VC++ runtime is already present or bundled'
  }

  if ($OSInfo.IsWin7) {
    if ($UcrtUrl) {
      $ucrtPath = Join-Path $BootstrapRoot (Split-Path -Leaf $UcrtUrl)
      if (-not $ucrtPath -or $ucrtPath.EndsWith('\')) {
        $ucrtPath = Join-Path $BootstrapRoot 'ucrt.msu'
      }
      Download-File $UcrtUrl $ucrtPath
      Assert-Sha256 $ucrtPath $UcrtSha256 8
      Invoke-CheckedProcess 'wusa.exe' @("`"$ucrtPath`"", '/quiet', '/norestart') 8 'UCRT update'
    } else {
      Write-Log 'WARN: Win7 detected but ucrt_url not provided; assuming UCRT is already installed'
    }
  }
}

function Extract-Package([string]$PackagePath, [string]$StageDir) {
  if (Test-Path $StageDir) { Remove-Item -Recurse -Force $StageDir }
  New-Item -ItemType Directory -Force -Path $StageDir | Out-Null
  $ext = [System.IO.Path]::GetExtension($PackagePath).ToLowerInvariant()
  if ($ext -eq '.zip') {
    Write-Log ('Extracting zip package ' + $PackagePath)
    try {
      Add-Type -AssemblyName System.IO.Compression.FileSystem -ErrorAction Stop
      [System.IO.Compression.ZipFile]::ExtractToDirectory($PackagePath, $StageDir)
    } catch {
      Write-Log ('ZipFile unavailable, falling back to Shell.Application: ' + $_.Exception.Message)
      $shell = New-Object -ComObject Shell.Application
      $zip = $shell.NameSpace($PackagePath)
      $dest = $shell.NameSpace($StageDir)
      if ($zip -eq $null -or $dest -eq $null) {
        Fail 6 'Shell.Application could not open zip package'
      }
      $dest.CopyHere($zip.Items(), 0x14)
      Start-Sleep -Seconds 3
    }
  } elseif ($ext -eq '.exe') {
    Copy-Item -LiteralPath $PackagePath -Destination (Join-Path $StageDir 'device-agent.exe') -Force
  } else {
    Fail 6 ('unsupported package type: ' + $ext)
  }
}

function Find-AgentExe([string]$StageDir) {
  $hit = Get-ChildItem -LiteralPath $StageDir -Recurse -Filter 'device-agent.exe' | Select-Object -First 1
  if ($hit -eq $null) {
    Fail 6 'package does not contain device-agent.exe'
  }
  return $hit.FullName
}

function Install-AgentFiles([string]$AgentExe, [string]$TargetDir) {
  $sourceDir = Split-Path -Parent $AgentExe
  if (-not (Test-Path $TargetDir)) {
    New-Item -ItemType Directory -Force -Path $TargetDir | Out-Null
  }
  Write-Log ('Copying agent files from ' + $sourceDir + ' to ' + $TargetDir)
  Copy-Item -Path (Join-Path $sourceDir '*') -Destination $TargetDir -Recurse -Force
}

function Remove-ServiceIfExists([string]$Name) {
  $svc = Get-Service -Name $Name -ErrorAction SilentlyContinue
  if ($svc) {
    Write-Log ('Removing existing service ' + $Name)
    if ($svc.Status -ne 'Stopped') {
      try { Stop-Service -Name $Name -Force -ErrorAction SilentlyContinue } catch {}
    }
    & sc.exe delete $Name | Out-Null
    for ($i = 0; $i -lt 30; $i++) {
      if (-not (Get-Service -Name $Name -ErrorAction SilentlyContinue)) { return }
      Start-Sleep -Milliseconds 500
    }
    Fail 7 ('service did not disappear after delete: ' + $Name)
  }
}

function Write-AgentConfig([string]$Path) {
  $json = @"
{
  "device_id": "$(ConvertTo-JsonString $DeviceId)",
  "token": "$(ConvertTo-JsonString $Token)",
  "server_host": "$(ConvertTo-JsonString $ServerHost)",
  "server_port": $ServerPort,
  "use_tls": $UseTls,
  "heartbeat_interval": 30,
  "heartbeat_timeout": 5,
  "status_interval": 300,
  "reconnect_base": 1,
  "reconnect_max": 60,
  "log_level": "info",
  "log_file": "$(ConvertTo-JsonString (Join-Path $LogDir 'device-agent.log'))",
  "business_bridge_type": "null"
}
"@
  Set-Content -LiteralPath $Path -Value $json -Encoding UTF8
}

function Install-Service([string]$Name, [string]$ExePath, [string]$ConfigPath) {
  Remove-ServiceIfExists $Name
  $binPath = ('"{0}" --service --service-name "{1}" -c "{2}"' -f $ExePath, $Name, $ConfigPath)
  Write-Log ('Creating service ' + $Name)
  & sc.exe create $Name binPath= $binPath start= auto DisplayName= 'DeviceOps device-agent' | Out-Null
  if ($LASTEXITCODE -ne 0) {
    Fail 7 ('sc create failed for ' + $Name)
  }
  & sc.exe failure $Name reset= 60 actions= restart/5000/restart/30000/restart/60000 | Out-Null
  & sc.exe description $Name 'DeviceOps device-agent service' | Out-Null
  Start-Service -Name $Name
  Start-Sleep -Seconds 2
  $svc = Get-Service -Name $Name -ErrorAction Stop
  if ($svc.Status -ne 'Running') {
    Fail 7 ('service not running after start: ' + $svc.Status)
  }
  Write-Log ('Service running: ' + $Name)
}

function Send-BootstrapReport([string]$Status, [string]$Message) {
  if (-not $BootstrapReportUrl) { return }
  try {
    $body = ('{{"device_id":"{0}","status":"{1}","message":"{2}","service_name":"{3}"}}' -f
      (ConvertTo-JsonString $DeviceId),
      (ConvertTo-JsonString $Status),
      (ConvertTo-JsonString $Message),
      (ConvertTo-JsonString $ServiceName))
    $wc = New-Object System.Net.WebClient
    try {
      $wc.Headers.Add('Content-Type', 'application/json')
      $wc.UploadString($BootstrapReportUrl, 'POST', $body) | Out-Null
      Write-Log ('Bootstrap report sent: ' + $Status)
    } finally {
      $wc.Dispose()
    }
  } catch {
    Write-Log ('WARN: bootstrap report failed: ' + $_.Exception.Message)
  }
}

try {
  Write-Log '=== DeviceOps device-agent bootstrap starting ==='
  if (-not (Test-IsAdmin)) {
    Fail 2 'administrator privilege is required'
  }

  if (-not $AnswerFile) {
    $candidate = Join-Path $ScriptDir 'device-agent.answer'
    if (Test-Path $candidate) { $AnswerFile = $candidate }
  }
  $answer = @{}
  if ($AnswerFile) {
    if (-not (Test-Path $AnswerFile)) { Fail 3 ('answer file not found: ' + $AnswerFile) }
    Write-Log ('Loading answer file ' + $AnswerFile)
    $answer = Read-AnswerFile $AnswerFile
  }

  $ServerHost = Pick-Value $answer @('server_host', 'host') $ServerHost ''
  $ServerPort = Pick-Int $answer @('server_port', 'port') $ServerPort 9090
  $UseTls = To-BoolString (Pick-Value $answer @('use_tls', 'tls') $UseTls 'false') 'false'
  $DeviceId = Pick-Value $answer @('device_id', 'device') $DeviceId ''
  $Token = Pick-Value $answer @('token', 'device_token') $Token ''
  $AgentUrl = Pick-Value $answer @('agent_url', 'package_url', 'download_url') $AgentUrl ''
  $AgentSha256 = Pick-Value $answer @('agent_sha256', 'package_sha256', 'sha256') $AgentSha256 ''
  $InstallDir = Pick-Value $answer @('install_dir') $InstallDir $DefaultInstallDir
  $ServiceName = Pick-Value $answer @('service_name') $ServiceName 'DeviceAgent'
  $LogDir = Pick-Value $answer @('log_dir') $LogDir (Join-Path $env:ProgramData 'DeviceOps\device-agent\logs')
  $VcRedistUrl = Pick-Value $answer @('vc_redist_url') $VcRedistUrl ''
  $VcRedistSha256 = Pick-Value $answer @('vc_redist_sha256') $VcRedistSha256 ''
  $UcrtUrl = Pick-Value $answer @('ucrt_url') $UcrtUrl ''
  $UcrtSha256 = Pick-Value $answer @('ucrt_sha256') $UcrtSha256 ''
  $BootstrapReportUrl = Pick-Value $answer @('bootstrap_report_url', 'report_url') $BootstrapReportUrl ''

  if (-not $ServiceName) { $ServiceName = 'DeviceAgent' }
  if (-not $InstallDir) { $InstallDir = $DefaultInstallDir }
  if (-not $LogDir) { $LogDir = Join-Path $env:ProgramData 'DeviceOps\device-agent\logs' }

  if ($Uninstall) {
    Remove-ServiceIfExists $ServiceName
    if (Test-Path $InstallDir) { Remove-Item -Recurse -Force $InstallDir }
    Write-Log ('Uninstalled ' + $ServiceName)
    exit 0
  }

  if (-not $ServerHost -or -not $DeviceId -or -not $Token -or -not $AgentUrl -or -not $AgentSha256) {
    Fail 3 'missing required server_host/device_id/token/agent_url/agent_sha256'
  }

  $os = Get-OSInfo
  Write-Log ('OS=' + $os.Caption + ' version=' + $os.Version + ' arch=' + $os.OSArchitecture + ' cpu=' + $os.AddressWidth)
  if ($os.AddressWidth -ne 64) {
    Fail 3 'only x64 Windows is supported by this bootstrap package'
  }

  Install-Prerequisites $os

  if (-not (Test-Path $LogDir)) {
    New-Item -ItemType Directory -Force -Path $LogDir | Out-Null
  }

  $packageLeaf = Split-Path -Leaf ([Uri]$AgentUrl).AbsolutePath
  if (-not $packageLeaf) { $packageLeaf = 'device-agent-package.zip' }
  $packagePath = Join-Path $BootstrapRoot $packageLeaf
  Download-File $AgentUrl $packagePath
  Assert-Sha256 $packagePath $AgentSha256 5

  $stageDir = Join-Path $BootstrapRoot 'stage'
  Extract-Package $packagePath $stageDir
  $agentExe = Find-AgentExe $stageDir
  Remove-ServiceIfExists $ServiceName
  Install-AgentFiles $agentExe $InstallDir

  $configPath = Join-Path $InstallDir 'config.json'
  Write-AgentConfig $configPath

  $installedExe = Join-Path $InstallDir 'device-agent.exe'
  if (-not (Test-Path $installedExe)) {
    Fail 4 ('installed exe not found: ' + $installedExe)
  }
  Install-Service $ServiceName $installedExe $configPath
  Send-BootstrapReport 'installed' 'service started'
  Write-Log '=== DeviceOps device-agent bootstrap success ==='
  exit 0
} catch {
  Send-BootstrapReport 'failed' $_.Exception.Message
  Fail 1 $_.Exception.Message
}
