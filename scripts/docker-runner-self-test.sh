#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RUNNER_BIN="${RUNNER_BIN:-}"
if [[ -z "$RUNNER_BIN" ]]; then
  if [[ -x "$ROOT_DIR/build-linux/bin/p2p_docker_runner" ]]; then
    RUNNER_BIN="$ROOT_DIR/build-linux/bin/p2p_docker_runner"
  elif [[ -x "/tmp/build-p2p/bin/p2p_docker_runner" ]]; then
    RUNNER_BIN="/tmp/build-p2p/bin/p2p_docker_runner"
  else
    echo "p2p_docker_runner not found; set RUNNER_BIN or build build-linux first" >&2
    exit 2
  fi
fi

WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/p2p-runner-self-test.XXXXXX")"
cleanup() {
  if [[ -n "${HTTP_PID:-}" ]]; then
    kill "$HTTP_PID" 2>/dev/null || true
  fi
  rm -rf "$WORK_DIR"
}
trap cleanup EXIT

"$RUNNER_BIN" --self-test >/dev/null
if "$RUNNER_BIN" --log-format xml --self-test >/dev/null 2>&1; then
  echo "--self-test accepted invalid --log-format" >&2
  exit 1
fi

python3 - "$WORK_DIR" <<'PY'
import hashlib
import os
import sys

root = sys.argv[1]
payload_path = os.path.join(root, "payload.bin")
data = (b"device-agent-p2p-runner-self-test\n" * 65536)[:2 * 1024 * 1024]
with open(payload_path, "wb") as f:
    f.write(data)

piece_length = 262144
pieces = b"".join(hashlib.sha1(data[i:i + piece_length]).digest()
                  for i in range(0, len(data), piece_length))

def bencode(value):
    if isinstance(value, int):
        return b"i" + str(value).encode() + b"e"
    if isinstance(value, bytes):
        return str(len(value)).encode() + b":" + value
    if isinstance(value, str):
        return bencode(value.encode())
    if isinstance(value, list):
        return b"l" + b"".join(bencode(v) for v in value) + b"e"
    if isinstance(value, dict):
        out = b"d"
        for key in sorted(value):
            out += bencode(key) + bencode(value[key])
        return out + b"e"
    raise TypeError(type(value))

torrent = {
    "announce": "http://127.0.0.1:9/announce",
    "creation date": 0,
    "info": {
        "length": len(data),
        "name": "payload.bin",
        "piece length": piece_length,
        "pieces": pieces,
    },
}
with open(os.path.join(root, "payload.torrent"), "wb") as f:
    f.write(bencode(torrent))
with open(os.path.join(root, "sha256.txt"), "w", encoding="utf-8") as f:
    f.write(hashlib.sha256(data).hexdigest())
PY

PORT="$(python3 - <<'PY'
import socket
s = socket.socket()
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])
s.close()
PY
)"
python3 -m http.server "$PORT" --bind 127.0.0.1 --directory "$WORK_DIR" >/dev/null 2>&1 &
HTTP_PID="$!"
sleep 1

DEST_DIR="$WORK_DIR/download"
mkdir -p "$DEST_DIR"
SHA256="$(cat "$WORK_DIR/sha256.txt")"
LOG_FILE="$WORK_DIR/runner.jsonl"
"$RUNNER_BIN" \
  --torrent "$WORK_DIR/payload.torrent" \
  --dest "$DEST_DIR" \
  --url "http://127.0.0.1:${PORT}/payload.bin" \
  --sha256 "$SHA256" \
  --file-size "$((2 * 1024 * 1024))" \
  --keep-seeding-seconds 0 \
  --log-format json \
  --runner-id self-test \
  --tracker-url "http://tracker:6969/announce" \
  --peer-wait-seconds 1 \
  --retry-count 1 \
  --metric-port 0 \
  >"$LOG_FILE"

if command -v jq >/dev/null 2>&1; then
  jq -e 'select(.runner_id and .timestamp and .event and .payload)' "$LOG_FILE" >/dev/null
fi
if command -v sha256sum >/dev/null 2>&1; then
  printf '%s  %s\n' "$SHA256" "$DEST_DIR/payload.bin" | sha256sum -c -
else
  printf '%s  %s\n' "$SHA256" "$DEST_DIR/payload.bin" | shasum -a 256 -c -
fi
grep -q 'reserved_cli_not_implemented' "$LOG_FILE"
echo "docker-runner-self-test PASS"
