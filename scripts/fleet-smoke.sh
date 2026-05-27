#!/usr/bin/env bash
set -euo pipefail

usage() {
  echo "Usage: $0 --size <n>" >&2
}

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SIZE=10
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

if [[ ! "$SIZE" =~ ^[0-9]+$ || "$SIZE" -lt 1 ]]; then
  usage
  exit 2
fi

PROJECT="fleet-smoke-$$"
WORK_DIR="${TMPDIR:-/tmp}/device-agent-fleet-smoke-$PROJECT"
SHARED_DIR="$WORK_DIR/shared"
COMPOSE_FILE="$WORK_DIR/fleet.yml"
RUNNER_IMAGE="${RUNNER_IMAGE:-runner:s2}"
TRACKER_IMAGE="${TRACKER_IMAGE:-opentracker:s2}"
mkdir -p "$SHARED_DIR/downloads"

cleanup() {
  if [[ "${FLEET_SMOKE_KEEP:-0}" == "1" ]]; then
    echo "fleet-smoke keeping work dir: $WORK_DIR" >&2
    return
  fi
  docker rm -f "${PROJECT}-seed" >/dev/null 2>&1 || true
  docker compose -p "$PROJECT" -f "$COMPOSE_FILE" down -v >/dev/null 2>&1 || true
  rm -rf "$WORK_DIR"
}
trap cleanup EXIT

python3 - "$SHARED_DIR" <<'PY'
import hashlib
import os
import sys

root = sys.argv[1]
payload = (b"device-agent-fleet-smoke\n" * 262144)[:4 * 1024 * 1024]
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

docker build -q -f "$ROOT_DIR/docker/Dockerfile.tracker" -t "$TRACKER_IMAGE" "$ROOT_DIR" >/dev/null
docker build -q -f "$ROOT_DIR/docker/Dockerfile.p2p-runner" -t "$RUNNER_IMAGE" "$ROOT_DIR" >/dev/null

"$ROOT_DIR/bin/fleet-compose-gen.sh" \
  --size "$SIZE" \
  --shared-dir "$SHARED_DIR" \
  --runner-image "$RUNNER_IMAGE" \
  --tracker-image "$TRACKER_IMAGE" \
  --peer-wait-seconds 60 \
  --retry-count 3 \
  --keep-seeding-seconds 5 \
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
  --keep-seeding-seconds 300 \
  --tracker-url http://tracker:6969/announce >/dev/null

sleep 5
docker compose -p "$PROJECT" -f "$COMPOSE_FILE" up -d --scale runner="$SIZE" runner >/dev/null

deadline=$((SECONDS + 240))
while (( SECONDS < deadline )); do
  running="$(docker compose -p "$PROJECT" -f "$COMPOSE_FILE" ps runner --status running -q | wc -l | tr -d ' ')"
  [[ "$running" == "0" ]] && break
  sleep 2
done

runner_ids="$(docker compose -p "$PROJECT" -f "$COMPOSE_FILE" ps -a runner -q)"
completed=0
peer_positive=0
for id in $runner_ids; do
  status="$(docker inspect -f '{{.State.ExitCode}}' "$id")"
  [[ "$status" == "0" ]] && completed=$((completed + 1))
  if docker logs "$id" 2>&1 | grep -Eq 'from_peers=[1-9][0-9]*'; then
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
min_peer_positive=$(( (SIZE * 90 + 99) / 100 ))

echo "fleet-smoke completed=$completed/$SIZE sha_ok=$sha_ok/$SIZE peer_positive=$peer_positive/$SIZE announce_count=$announce_count"
if [[ "$completed" -ne "$SIZE" ]]; then
  echo "fleet-smoke FAIL: completed runners below target" >&2
  exit 1
fi
if [[ "$sha_ok" -ne "$SIZE" ]]; then
  echo "fleet-smoke FAIL: sha verified downloads below target" >&2
  exit 1
fi
if [[ "$peer_positive" -lt "$min_peer_positive" ]]; then
  echo "fleet-smoke FAIL: p2p-positive runners below 90 percent target" >&2
  exit 1
fi
if [[ "$announce_count" -lt "$SIZE" ]]; then
  echo "fleet-smoke FAIL: tracker announce count below target" >&2
  exit 1
fi
echo "fleet-smoke PASS"
