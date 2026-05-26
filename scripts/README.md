# Device-agent Scripts

## M3 Beta E2E Paths

`m3-beta-e2e.sh` writes results under `RESULT_ROOT`, defaulting to:

```bash
./logs/m3-beta-e2e
```

Override it when running from CI or the DeviceOps collaboration workspace:

```bash
RESULT_ROOT=/tmp/m3-beta-e2e bash scripts/m3-beta-e2e.sh --scenario=proto
```

The config-hotload scenario reads backend logs from `SERVER_LOG`, defaulting to:

```bash
/tmp/terminal-agent-dev.log
```

Common deployment templates:

```bash
# systemd service
SERVER_LOG=/var/log/terminal-agent-dev/terminal-agent-dev.log

# docker compose service
SERVER_LOG=/tmp/terminal-agent-dev.log
docker compose logs terminal-agent-dev > "$SERVER_LOG"

# FRP or remote host capture
SERVER_LOG=/tmp/terminal-agent-dev-frp.log
ssh "$FRP_HOST" 'journalctl -u terminal-agent-dev --since "-10 min" --no-pager' > "$SERVER_LOG"
```

## M3 Alpha Wait And Install

`m3-alpha-e2e.sh` uses a configurable wait window:

```bash
E2E_WAIT_SECONDS=300 bash scripts/m3-alpha-e2e.sh
```

The default is `600` seconds. After a P2P APK path appears in logcat, the script stages it to
`/sdcard/Download/m3-alpha` and runs:

```bash
adb shell pm install -r -d /sdcard/Download/m3-alpha/<apk>
```

Set `HEADLESS_PM_INSTALL=0` to disable that helper when testing the app's own installer flow.
