#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
Usage:
  bash scripts/simulate-network-switch.sh <serial> {wifi-off|wifi-on|data-off|data-on|to-wifi|to-cellular}

Commands:
  wifi-off       Disable Wi-Fi
  wifi-on        Enable Wi-Fi
  data-off       Disable mobile data
  data-on        Enable mobile data
  to-wifi        Enable Wi-Fi + disable mobile data
  to-cellular    Disable Wi-Fi + enable mobile data

Options:
  --wait N       Wait up to N seconds for network connectivity (ping 8.8.8.8)
  --help         Show this help

Examples:
  bash scripts/simulate-network-switch.sh bf9ec82f to-cellular
  bash scripts/simulate-network-switch.sh bf9ec82f to-wifi --wait 30

Notes:
  - Requires adb and android.permission.CHANGE_NETWORK_STATE
  - svc wifi enable/disable may be restricted on some OEM ROMs (MIUI, ColorOS)
  - svc data disable works on Android 5.0+; may require root on some devices
USAGE
}

SERIAL=""
ACTION=""
WAIT=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --wait) WAIT="$2"; shift 2 ;;
    --help|-h) usage; exit 0 ;;
    -*)
      echo "ERROR: unknown option $1" >&2; usage; exit 1 ;;
    *)
      if [[ -z "$SERIAL" ]]; then
        SERIAL="$1"; shift
      elif [[ -z "$ACTION" ]]; then
        ACTION="$1"; shift
      else
        echo "ERROR: unexpected argument $1" >&2; usage; exit 1
      fi ;;
  esac
done

[[ -n "$SERIAL" ]] || { echo "ERROR: serial required"; usage; exit 1; }
[[ -n "$ACTION" ]] || { echo "ERROR: action required"; usage; exit 1; }

adb -s "$SERIAL" shell getprop ro.product.model >/dev/null 2>&1 || {
  echo "ERROR: device $SERIAL not reachable"; exit 1
}

printf '[%s] device=%s action=%s\n' "$(date '+%H:%M:%S')" "$SERIAL" "$ACTION"

case "$ACTION" in
  wifi-off)
    echo "  → disabling Wi-Fi"
    adb -s "$SERIAL" shell svc wifi disable
    ;;
  wifi-on)
    echo "  → enabling Wi-Fi"
    adb -s "$SERIAL" shell svc wifi enable
    ;;
  data-off)
    echo "  → disabling mobile data"
    adb -s "$SERIAL" shell svc data disable
    ;;
  data-on)
    echo "  → enabling mobile data"
    adb -s "$SERIAL" shell svc data enable
    ;;
  to-wifi)
    echo "  → switching to Wi-Fi (enable Wi-Fi + disable mobile data)"
    adb -s "$SERIAL" shell svc wifi enable
    adb -s "$SERIAL" shell svc data disable
    ;;
  to-cellular)
    echo "  → switching to cellular (disable Wi-Fi + enable mobile data)"
    adb -s "$SERIAL" shell svc wifi disable
    adb -s "$SERIAL" shell svc data enable
    ;;
  *)
    echo "ERROR: unknown action $ACTION" >&2
    usage; exit 1
    ;;
esac

if (( WAIT > 0 )); then
  echo "  → waiting up to ${WAIT}s for network connectivity..."
  for ((i=0; i<WAIT; i++)); do
    if adb -s "$SERIAL" shell ping -c 1 -W 2 8.8.8.8 >/dev/null 2>&1; then
      echo "  → network is back after ${i}s"
      break
    fi
    sleep 1
  done
  if adb -s "$SERIAL" shell ping -c 1 -W 2 8.8.8.8 >/dev/null 2>&1; then
    echo "  → network OK"
  else
    echo "  → WARNING: network may still be unavailable after ${WAIT}s"
  fi
fi

echo "[simulate-network-switch] done"
