#!/usr/bin/env bash
set -euo pipefail

usage() {
  echo "Usage: $0 --size <n> [--compose-out <path>]" >&2
}

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SIZE=""
COMPOSE_OUT=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --size)
      SIZE="${2:-}"
      shift 2
      ;;
    --compose-out)
      COMPOSE_OUT="${2:-}"
      shift 2
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

if [[ -z "$SIZE" || ! "$SIZE" =~ ^[0-9]+$ || "$SIZE" -lt 1 ]]; then
  usage
  exit 2
fi

if [[ -z "$COMPOSE_OUT" ]]; then
  COMPOSE_OUT="${TMPDIR:-/tmp}/device-agent-fleet-${SIZE}.yml"
fi

"$ROOT_DIR/bin/fleet-compose-gen.sh" --size "$SIZE" > "$COMPOSE_OUT"
echo "Generated compose: $COMPOSE_OUT"
echo "S5 owns ramp thresholds/soak. Start manually with:"
echo "  docker compose -f $COMPOSE_OUT up -d --scale runner=$SIZE"
