#!/usr/bin/env bash
# Windows bootstrap 打包校验函数库的离线测试入口。
# 需要 PowerShell（Windows PowerShell 5.1 或 pwsh 7+）；本机无 PowerShell 时
# 输出 SKIP 并以 0 退出（B3 Windows VM gate 会真正执行）。
set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

if ! command -v pwsh >/dev/null 2>&1; then
    echo "SKIP: pwsh not found on this host; run under Windows/pwsh (B3 VM gate)."
    exit 0
fi

exec pwsh -NoProfile -File "$SCRIPT_DIR/windows-bootstrap/make-kit-lib.Tests.ps1"
