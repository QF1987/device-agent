#ifdef _WIN32

#include "remotedesktop/platform/windows/windows_badge.h"

#include <string>

namespace device_agent::remotedesktop::windows {

namespace {

const wchar_t* kBadgeCommand =
    L"powershell -NoProfile -Command "
    L"\"Add-Type -AssemblyName System.Windows.Forms;"
    L"Add-Type -AssemblyName System.Drawing;"
    L"$f=New-Object Windows.Forms.Form;"
    L"$f.Text='DeviceOps Remote Control';"
    L"$f.FormBorderStyle='None';$f.TopMost=$true;$f.ShowInTaskbar=$false;"
    L"$f.StartPosition='Manual';$f.ShowActivated=$false;$f.Opacity=0.88;"
    L"$f.BackColor=[System.Drawing.Color]::FromArgb(53,30,36);"
    L"$f.Width=330;$f.Height=44;"
    L"$wa=[System.Windows.Forms.Screen]::PrimaryScreen.WorkingArea;"
    L"$left=[Math]::Max(20,($wa.Width-$f.Width-20));"
    L"$f.Left=$left;$f.Top=20;"
    L"$gp=New-Object System.Drawing.Drawing2D.GraphicsPath;"
    L"$d=18;$w=$f.Width;$h=$f.Height;"
    L"$gp.AddArc(0,0,$d,$d,180,90);$gp.AddArc(($w-$d-1),0,$d,$d,270,90);"
    L"$gp.AddArc(($w-$d-1),($h-$d-1),$d,$d,0,90);$gp.AddArc(0,($h-$d-1),$d,$d,90,90);"
    L"$gp.CloseFigure();$f.Region=New-Object System.Drawing.Region($gp);"
    L"$l=New-Object Windows.Forms.Label;"
    L"$l.Text=([char]0x25cf+[char]0x20+[char]0x6b63+[char]0x5728+[char]0x88ab+[char]0x8fdc+[char]0x7a0b+[char]0x63a7+[char]0x5236);"
    L"$l.ForeColor=[System.Drawing.Color]::White;$l.BackColor=$f.BackColor;"
    L"$l.Font=New-Object System.Drawing.Font('Segoe UI',10,[System.Drawing.FontStyle]::Bold);"
    L"$l.AutoSize=$false;$l.Left=16;$l.Top=10;$l.Width=300;$l.Height=24;"
    L"$t=New-Object Windows.Forms.Timer;$t.Interval=900;"
    L"$t.Add_Tick({if($l.ForeColor -eq [System.Drawing.Color]::White){$l.ForeColor=[System.Drawing.Color]::FromArgb(255,196,196)}else{$l.ForeColor=[System.Drawing.Color]::White}});$t.Start();"
    L"$f.Add_FormClosing({param($s,$e)$e.Cancel=$true});"
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
