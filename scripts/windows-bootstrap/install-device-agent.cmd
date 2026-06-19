@echo off
setlocal EnableExtensions
chcp 65001 >nul

set "SCRIPT_DIR=%~dp0"
set "PS_SCRIPT=%SCRIPT_DIR%install-device-agent.ps1"

if not exist "%PS_SCRIPT%" (
  echo Missing PowerShell installer: "%PS_SCRIPT%"
  exit /b 4
)

net session >nul 2>&1
if not "%errorlevel%"=="0" (
  echo Requesting administrator privileges...
  powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath $env:ComSpec -ArgumentList '/c ""%~f0"" %*' -Verb RunAs -Wait"
  exit /b %errorlevel%
)

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%PS_SCRIPT%" %*
exit /b %errorlevel%
