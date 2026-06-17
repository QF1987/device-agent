#ifdef _WIN32

#include "remotedesktop/platform/windows/windows_badge.h"

#include <string>

namespace device_agent::remotedesktop::windows {

namespace {

const wchar_t* kBadgeCommand =
    L"powershell -NoProfile -Command "
    L"\"Add-Type -AssemblyName System.Windows.Forms;"
    L"$f=New-Object Windows.Forms.Form;"
    L"$f.Text='DeviceOps Remote Control';"
    L"$f.FormBorderStyle='None';$f.TopMost=$true;$f.ShowInTaskbar=$false;"
    L"$f.StartPosition='Manual';"
    L"$f.BackColor='DarkRed';$f.Width=360;$f.Height=36;"
    L"$wa=[System.Windows.Forms.Screen]::PrimaryScreen.WorkingArea;"
    L"$left=[Math]::Max(20,($wa.Width-$f.Width-20));"
    L"$f.Left=$left;$f.Top=20;"
    L"$l=New-Object Windows.Forms.Label;"
    L"$l.Text=([char]0x25cf+[char]0x20+[char]0x6b63+[char]0x5728+[char]0x88ab+[char]0x8fdc+[char]0x7a0b+[char]0x63a7+[char]0x5236);"
    L"$l.ForeColor='White';$l.BackColor='DarkRed';$l.AutoSize=$true;"
    L"$l.Left=12;$l.Top=8;"
    L"$f.Controls.Add($l);[Windows.Forms.Application]::Run($f)\"";

}  // namespace

WindowsRemoteControlBadge::~WindowsRemoteControlBadge() {
    hide();
}

bool WindowsRemoteControlBadge::show(std::string& err) {
    if (process_) {
        return true;
    }
    std::wstring command(kBadgeCommand);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    // Do not use PowerShell -WindowStyle: PowerShell 2.0 on Win7 rejects it.
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        err = "CreateProcessW badge failed";
        return false;
    }
    CloseHandle(pi.hThread);
    process_ = pi.hProcess;
    return true;
}

void WindowsRemoteControlBadge::hide() {
    if (!process_) {
        return;
    }
    TerminateProcess(process_, 0);
    CloseHandle(process_);
    process_ = nullptr;
}

}  // namespace device_agent::remotedesktop::windows

#endif  // _WIN32
