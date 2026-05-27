#!/usr/bin/env bash
set -euo pipefail

usage() {
  echo "Usage: $0 --size <n> --interval <seconds> --soak-seconds <seconds> [--compose-file <path>] [--project <name>]" >&2
}

SIZE=""
INTERVAL=30
SOAK_SECONDS=7200
COMPOSE_FILE=""
PROJECT=""

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
    --interval)
      INTERVAL="${2:-}"
      shift 2
      ;;
    --interval=*)
      INTERVAL="${1#*=}"
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
    --compose-file)
      COMPOSE_FILE="${2:-}"
      shift 2
      ;;
    --compose-file=*)
      COMPOSE_FILE="${1#*=}"
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
      ! "$INTERVAL" =~ ^[0-9]+$ || "$INTERVAL" -lt 1 ||
      ! "$SOAK_SECONDS" =~ ^[0-9]+$ || "$SOAK_SECONDS" -lt 1 ]]; then
  usage
  exit 2
fi

if [[ -z "$COMPOSE_FILE" ]]; then
  COMPOSE_FILE="${TMPDIR:-/tmp}/fleet-${SIZE}.yml"
fi
if [[ -z "$PROJECT" ]]; then
  PROJECT="$(basename "$COMPOSE_FILE")"
  PROJECT="${PROJECT%.*}"
fi

if [[ ! -f "$COMPOSE_FILE" ]]; then
  echo "compose file not found: $COMPOSE_FILE" >&2
  exit 2
fi

min_peer_positive=$(( (SIZE * 90 + 99) / 100 ))
deadline=$((SECONDS + SOAK_SECONDS))
sample=0

echo "# fleet-monitor size=${SIZE} interval=${INTERVAL} soak=${SOAK_SECONDS} compose=${COMPOSE_FILE} project=${PROJECT}"
echo "timestamp,sample,running,exited_zero,peer_positive,completed,sha_ok,cpu_percent_sum,mem_percent_sum,verdict"

while (( SECONDS < deadline )); do
  sample=$((sample + 1))
  timestamp="$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
  runner_ids="$(docker compose -p "$PROJECT" -f "$COMPOSE_FILE" ps -a runner -q 2>/dev/null || true)"
  running=0
  exited_zero=0
  peer_positive=0
  completed=0
  sha_ok=0

  for id in $runner_ids; do
    log_text="$(docker logs "$id" 2>&1 || true)"
    state="$(docker inspect -f '{{.State.Status}}' "$id" 2>/dev/null || true)"
    exit_code="$(docker inspect -f '{{.State.ExitCode}}' "$id" 2>/dev/null || true)"
    [[ "$state" == "running" ]] && running=$((running + 1))
    [[ "$exit_code" == "0" ]] && exited_zero=$((exited_zero + 1))
    if [[ "$(printf '%s\n' "$log_text" | grep -Ec 'from_peers=[1-9][0-9]*')" -gt 0 ]]; then
      peer_positive=$((peer_positive + 1))
    fi
    if [[ "$(printf '%s\n' "$log_text" | grep -c '"event":"download_complete"')" -gt 0 ]]; then
      completed=$((completed + 1))
    fi
    if [[ "$(printf '%s\n' "$log_text" | grep -c '"event":"sha256_verified"')" -gt 0 ]]; then
      sha_ok=$((sha_ok + 1))
    fi
  done

  stats=""
  if [[ -n "$runner_ids" ]]; then
    stats="$(docker stats --no-stream --format '{{.CPUPerc}} {{.MemPerc}}' $runner_ids 2>/dev/null || true)"
  fi
  cpu_sum="$(printf '%s\n' "$stats" | awk '{gsub(/%/,"",$1); s+=$1} END {printf "%.2f", s+0}')"
  mem_sum="$(printf '%s\n' "$stats" | awk '{gsub(/%/,"",$2); s+=$2} END {printf "%.2f", s+0}')"

  verdict="PASS"
  if [[ "$running" -ne "$SIZE" && "$completed" -ne "$SIZE" ]]; then
    verdict="WARN_CONTAINER_COUNT"
  elif [[ "$peer_positive" -lt "$min_peer_positive" ]]; then
    verdict="WARN_PEER_MESH"
  fi

  echo "${timestamp},${sample},${running},${exited_zero},${peer_positive},${completed},${sha_ok},${cpu_sum},${mem_sum},${verdict}"
  sleep "$INTERVAL"
done
