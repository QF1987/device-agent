#!/usr/bin/env bash
# 端到端冒烟测试：CLI 下发命令 → DB 记录 → 设备 poll → 设备 report → DB 状态更新
# 前置：terminal-agent-serve 正在跑（localhost:8080）、Postgres 可连
set -euo pipefail

TERMINAL_AGENT="/Users/qf/.openclaw/workspace/terminal-agent"
DEVICE_ID="SMOKE-TEST-$(date +%s)"
SERVE_URL="${SERVE_URL:-http://localhost:8080}"
PGPASSWORD="${PGPASSWORD:-deviceops123}"

cleanup() {
    PGPASSWORD="$PGPASSWORD" psql -h localhost -U deviceops -d deviceops \
        -c "DELETE FROM commands WHERE device_id='$DEVICE_ID'; DELETE FROM devices WHERE id='$DEVICE_ID';" >/dev/null 2>&1 || true
}
trap cleanup EXIT

echo "==> 1. 注册测试设备"
PGPASSWORD="$PGPASSWORD" psql -h localhost -U deviceops -d deviceops -c \
    "INSERT INTO devices (id, name, region, status, version) VALUES ('$DEVICE_ID', 'smoke-test', 'test', 'online', '1.0.0');" 2>/dev/null

echo "==> 2. 写入 update_config 命令到 DB（模拟 batch config）"
PGPASSWORD="$PGPASSWORD" psql -h localhost -U deviceops -d deviceops -c \
    "INSERT INTO commands (id, command_id, device_id, command_type, payload_json, status, timeout_seconds, issued_at, created_by)
     VALUES (gen_random_uuid()::text, 'smoke-cmd-'$(date +%s)', '$DEVICE_ID', 'update_config', '{\"key\":\"poll_interval_ms\",\"value\":\"5000\"}', 'pending', 60, NOW(), 'smoke-test');" 2>/dev/null

echo "==> 3. 查询 DB 确认 pending"
STATUS=$(PGPASSWORD="$PGPASSWORD" psql -h localhost -U deviceops -d deviceops -tAc \
    "SELECT status FROM commands WHERE device_id='$DEVICE_ID' ORDER BY id DESC LIMIT 1;")
STATUS=$(echo "$STATUS" | tr -d ' \n')
[[ "$STATUS" == "pending" ]] || { echo "FAIL: status=$STATUS expected pending"; exit 1; }

echo "==> 4. 模拟设备 poll"
POLL_RESP=$(curl -s "$SERVE_URL/api/v1/devices/$DEVICE_ID/poll")
echo "Poll response: $POLL_RESP"
CMD_ID=$(echo "$POLL_RESP" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d['commands'][0]['command_id']) if d.get('commands') else exit(1)" 2>/dev/null) || {
    echo "FAIL: no command_id in poll response"; exit 1;
}
[[ -n "$CMD_ID" ]] || { echo "FAIL: command_id is empty"; exit 1; }
echo "Got command_id: $CMD_ID"

echo "==> 5. 模拟设备回报 completed"
curl -s -X POST "$SERVE_URL/api/v1/devices/$DEVICE_ID/report" \
    -H "Content-Type: application/json" \
    -d "{\"command_id\":\"$CMD_ID\",\"status\":\"completed\",\"result_message\":\"smoke test ok\"}" >/dev/null

echo "==> 6. 查询 DB 确认 completed"
sleep 1
STATUS=$(PGPASSWORD="$PGPASSWORD" psql -h localhost -U deviceops -d deviceops -tAc \
    "SELECT status FROM commands WHERE command_id='$CMD_ID';")
STATUS=$(echo "$STATUS" | tr -d ' \n')
[[ "$STATUS" == "completed" ]] || { echo "FAIL: status=$STATUS expected completed"; exit 1; }

echo "==> ✅ PASS"
