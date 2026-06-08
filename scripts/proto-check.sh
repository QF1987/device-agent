#!/bin/bash
# ============================================================
# proto-check.sh — Proto 一致性校验脚本
#
# 用法：./scripts/proto-check.sh
# 退出码：0 = 一致，1 = 漂移
# ============================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DA_REPO="${DEVICE_AGENT_REPO:-$(cd "$SCRIPT_DIR/.." && pwd)}"
TA_REPO="${TERMINAL_AGENT_REPO:-$(cd "$DA_REPO/../terminal-agent-dev" && pwd)}"

DA_PROTO="$DA_REPO/proto/terminal_agent/v1"
TA_PROTO="$TA_REPO/proto/terminal_agent/v1"

RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m'

failures=0

echo "terminal-agent proto: $TA_PROTO"
echo "device-agent proto:   $DA_PROTO"
echo

for f in device.proto service.proto; do
    ta_hash=$(shasum "$TA_PROTO/$f" | awk '{print $1}')
    da_hash=$(shasum "$DA_PROTO/$f" | awk '{print $1}')
    if [ "$ta_hash" != "$da_hash" ]; then
        echo -e "${RED}DRIFT: $f${NC}"
        echo "  terminal-agent: $ta_hash"
        echo "  device-agent:   $da_hash"
        failures=$((failures + 1))
    else
        echo -e "${GREEN}OK: $f${NC} ($ta_hash)"
    fi
done

if [ $failures -gt 0 ]; then
    echo -e "\n${RED}$failures file(s) out of sync — do NOT proceed with builds${NC}"
    exit 1
fi

echo -e "\n${GREEN}All proto files in sync${NC}"
