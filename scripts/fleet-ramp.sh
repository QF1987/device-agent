#!/usr/bin/env bash
set -euo pipefail

usage() {
  echo "Usage: $0 --size <n> [--soak-seconds <seconds>] [--interval <seconds>] [--work-dir <dir>] [--payload-mib <mib>] [--project <name>]" >&2
}

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SIZE=""
SOAK_SECONDS=7200
INTERVAL=30
PAYLOAD_MIB=64
WORK_DIR=""
PROJECT=""
RUNNER_IMAGE="${RUNNER_IMAGE:-runner:s2}"
TRACKER_IMAGE="${TRACKER_IMAGE:-opentracker:s2}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --size)
      SIZE="${2:-}"
      shift 2
      ;;
    --size=*)
      SIZE="${1#*=}"
      shift
      ;;
    --soak-seconds)
      SOAK_SECONDS="${2:-}"
      shift 2
      ;;
    --soak-seconds=*)
      SOAK_SECONDS="${1#*=}"
      shift
      ;;
    --interval)
      INTERVAL="${2:-}"
      shift 2
      ;;
    --interval=*)
      INTERVAL="${1#*=}"
      shift
      ;;
    --payload-mib)
      PAYLOAD_MIB="${2:-}"
      shift 2
      ;;
    --payload-mib=*)
      PAYLOAD_MIB="${1#*=}"
      shift
      ;;
    --work-dir)
      WORK_DIR="${2:-}"
      shift 2
      ;;
    --work-dir=*)
      WORK_DIR="${1#*=}"
      shift
      ;;
    --project)
      PROJECT="${2:-}"
      shift 2
      ;;
    --project=*)
      PROJECT="${1#*=}"
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "unknown argument: $1" >&2
      usage
      exit 2
      ;;
  esac
done

if [[ -z "$SIZE" || ! "$SIZE" =~ ^[0-9]+$ || "$SIZE" -lt 1 ||
      ! "$SOAK_SECONDS" =~ ^[0-9]+$ || "$SOAK_SECONDS" -lt 1 ||
      ! "$INTERVAL" =~ ^[0-9]+$ || "$INTERVAL" -lt 1 ||
      ! "$PAYLOAD_MIB" =~ ^[0-9]+$ || "$PAYLOAD_MIB" -lt 1 ]]; then
  usage
  exit 2
fi

if [[ -z "$PROJECT" ]]; then
  PROJECT="fleet-ramp-${SIZE}-$$"
fi
if [[ -z "$WORK_DIR" ]]; then
  WORK_DIR="${TMPDIR:-/tmp}/device-agent-${PROJECT}"
fi

SHARED_DIR="$WORK_DIR/shared"
COMPOSE_FILE="$WORK_DIR/fleet.yml"
MONITOR_LOG="$WORK_DIR/monitor.csv"
SUMMARY_FILE="$WORK_DIR/summary.md"
if [[ -e "$WORK_DIR" && "${FLEET_RAMP_REUSE_WORKDIR:-0}" != "1" ]]; then
  echo "work dir already exists: $WORK_DIR" >&2
  echo "set FLEET_RAMP_REUSE_WORKDIR=1 to reuse it, or pass a fresh --work-dir" >&2
  exit 2
fi
mkdir -p "$SHARED_DIR/downloads"

cleanup() {
  if [[ "${FLEET_RAMP_KEEP:-0}" == "1" ]]; then
    echo "fleet-ramp keeping work dir: $WORK_DIR" >&2
    return
  fi
  docker rm -f "${PROJECT}-seed" >/dev/null 2>&1 || true
  docker compose -p "$PROJECT" -f "$COMPOSE_FILE" down -v >/dev/null 2>&1 || true
}
trap cleanup EXIT

python3 - "$SHARED_DIR" "$PAYLOAD_MIB" <<'PY'
import hashlib
import os
import sys

root = sys.argv[1]
payload_size = int(sys.argv[2]) * 1024 * 1024
chunk = b"device-agent-fleet-ramp\n"
payload = (chunk * ((payload_size // len(chunk)) + 1))[:payload_size]
piece_length = 262144
pieces = b"".join(hashlib.sha1(payload[i:i + piece_length]).digest()
                  for i in range(0, len(payload), piece_length))

def bencode(value):
    if isinstance(value, int):
        return b"i" + str(value).encode() + b"e"
    if isinstance(value, bytes):
        return str(len(value)).encode() + b":" + value
    if isinstance(value, str):
        return bencode(value.encode())
    if isinstance(value, dict):
        return b"d" + b"".join(bencode(k) + bencode(value[k]) for k in sorted(value)) + b"e"
    raise TypeError(type(value))

open(os.path.join(root, "payload.bin"), "wb").write(payload)
open(os.path.join(root, "payload.torrent"), "wb").write(bencode({
    "announce": "http://tracker:6969/announce",
    "info": {
        "length": len(payload),
        "name": "payload.bin",
        "piece length": piece_length,
        "pieces": pieces,
    },
}))
open(os.path.join(root, "sha256.txt"), "w", encoding="utf-8").write(hashlib.sha256(payload).hexdigest())
open(os.path.join(root, "file_size.txt"), "w", encoding="utf-8").write(str(len(payload)))
PY

if [[ "${FLEET_RAMP_SKIP_BUILD:-0}" != "1" ]]; then
  docker build -q -f "$ROOT_DIR/docker/Dockerfile.tracker" -t "$TRACKER_IMAGE" "$ROOT_DIR" >/dev/null
  docker build -q -f "$ROOT_DIR/docker/Dockerfile.p2p-runner" -t "$RUNNER_IMAGE" "$ROOT_DIR" >/dev/null
fi

"$ROOT_DIR/bin/fleet-compose-gen.sh" \
  --size "$SIZE" \
  --shared-dir "$SHARED_DIR" \
  --runner-image "$RUNNER_IMAGE" \
  --tracker-image "$TRACKER_IMAGE" \
  --peer-wait-seconds 60 \
  --retry-count 3 \
  --keep-seeding-seconds "$SOAK_SECONDS" \
  > "$COMPOSE_FILE"

mkdir -p "$SHARED_DIR/seed"
cp "$SHARED_DIR/payload.bin" "$SHARED_DIR/seed/payload.bin"

docker compose -p "$PROJECT" -f "$COMPOSE_FILE" up -d tracker >/dev/null
NETWORK="${PROJECT}_fleet"
docker run -d --name "${PROJECT}-seed" --network "$NETWORK" \
  -v "$SHARED_DIR":/shared "$RUNNER_IMAGE" \
  --log-format=json \
  --runner-id seed \
  --torrent /shared/payload.torrent \
  --dest /shared/seed \
  --sha256 "$(cat "$SHARED_DIR/sha256.txt")" \
  --file-size "$(cat "$SHARED_DIR/file_size.txt")" \
  --keep-seeding-seconds "$((SOAK_SECONDS + 600))" \
  --tracker-url http://tracker:6969/announce >/dev/null

sleep 5
docker compose -p "$PROJECT" -f "$COMPOSE_FILE" up -d --scale runner="$SIZE" runner >/dev/null

"$ROOT_DIR/scripts/fleet-monitor.sh" \
  --size "$SIZE" \
  --interval "$INTERVAL" \
  --soak-seconds "$SOAK_SECONDS" \
  --compose-file "$COMPOSE_FILE" \
  --project "$PROJECT" | tee "$MONITOR_LOG"

runner_ids="$(docker compose -p "$PROJECT" -f "$COMPOSE_FILE" ps -a runner -q)"
completed=0
peer_positive=0
for id in $runner_ids; do
  log_text="$(docker logs "$id" 2>&1 || true)"
  status="$(docker inspect -f '{{.State.ExitCode}}' "$id")"
  [[ "$status" == "0" ]] && completed=$((completed + 1))
  if [[ "$(printf '%s\n' "$log_text" | grep -Ec 'from_peers=[1-9][0-9]*')" -gt 0 ]]; then
    peer_positive=$((peer_positive + 1))
  fi
done

expected_sha="$(cat "$SHARED_DIR/sha256.txt")"
sha_ok=0
for file in "$SHARED_DIR"/downloads/*/payload.bin; do
  [[ -f "$file" ]] || continue
  actual="$(shasum -a 256 "$file" | awk '{print $1}')"
  [[ "$actual" == "$expected_sha" ]] && sha_ok=$((sha_ok + 1))
done

announce_count="$(docker compose -p "$PROJECT" -f "$COMPOSE_FILE" logs tracker 2>&1 | grep -c '/announce' || true)"
sample_count="$(awk -F, 'NR > 2 {count++} END {print count + 0}' "$MONITOR_LOG")"
pass_sample_count="$(awk -F, 'NR > 2 && $10 == "PASS" {count++} END {print count + 0}' "$MONITOR_LOG")"
settled_sample_count="$(awk -F, -v size="$SIZE" 'NR > 2 && $6 == size {count++} END {print count + 0}' "$MONITOR_LOG")"
settled_pass_sample_count="$(awk -F, -v size="$SIZE" 'NR > 2 && $6 == size && $10 == "PASS" {count++} END {print count + 0}' "$MONITOR_LOG")"
min_peer_positive=$(( (SIZE * 90 + 99) / 100 ))
verdict="PASS"
if [[ "$completed" -ne "$SIZE" || "$sha_ok" -ne "$SIZE" || "$peer_positive" -lt "$min_peer_positive" || "$announce_count" -lt "$SIZE" ||
      "$settled_sample_count" -lt 1 || "$settled_sample_count" -ne "$settled_pass_sample_count" ]]; then
  verdict="FAIL"
fi

{
  echo "# Fleet Ramp Summary"
  echo
  echo "- size: $SIZE"
  echo "- verdict: $verdict"
  echo "- payload_mib: $PAYLOAD_MIB"
  echo "- soak_seconds: $SOAK_SECONDS"
  echo "- completed: $completed/$SIZE"
  echo "- sha_ok: $sha_ok/$SIZE"
  echo "- peer_positive: $peer_positive/$SIZE"
  echo "- announce_count: $announce_count"
  echo "- monitor_pass_samples: $pass_sample_count/$sample_count"
  echo "- monitor_settled_pass_samples: $settled_pass_sample_count/$settled_sample_count"
  echo "- work_dir: $WORK_DIR"
  echo "- compose_file: $COMPOSE_FILE"
  echo "- monitor_log: $MONITOR_LOG"
} | tee "$SUMMARY_FILE"

echo "fleet-ramp summary: $SUMMARY_FILE"
[[ "$verdict" == "PASS" ]]
