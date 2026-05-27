#!/usr/bin/env bash
set -euo pipefail

usage() {
  echo "Usage: $0 --size <n> [--shared-dir <dir>] [--runner-image <image>] [--tracker-image <image>] [--web-seed-url <url>]" >&2
}

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SIZE=""
SHARED_DIR="${FLEET_SHARED_DIR:-/tmp/device-agent-fleet-smoke/shared}"
RUNNER_IMAGE="${RUNNER_IMAGE:-runner:s2}"
TRACKER_IMAGE="${TRACKER_IMAGE:-opentracker:s2}"
WEB_SEED_URL="${WEB_SEED_URL:-}"
PEER_WAIT_SECONDS="${PEER_WAIT_SECONDS:-60}"
RETRY_COUNT="${RETRY_COUNT:-3}"
KEEP_SEEDING_SECONDS="${KEEP_SEEDING_SECONDS:-30}"

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
    --shared-dir)
      SHARED_DIR="${2:-}"
      shift 2
      ;;
    --shared-dir=*)
      SHARED_DIR="${1#*=}"
      shift
      ;;
    --runner-image)
      RUNNER_IMAGE="${2:-}"
      shift 2
      ;;
    --runner-image=*)
      RUNNER_IMAGE="${1#*=}"
      shift
      ;;
    --tracker-image)
      TRACKER_IMAGE="${2:-}"
      shift 2
      ;;
    --tracker-image=*)
      TRACKER_IMAGE="${1#*=}"
      shift
      ;;
    --web-seed-url)
      WEB_SEED_URL="${2:-}"
      shift 2
      ;;
    --web-seed-url=*)
      WEB_SEED_URL="${1#*=}"
      shift
      ;;
    --peer-wait-seconds)
      PEER_WAIT_SECONDS="${2:-}"
      shift 2
      ;;
    --peer-wait-seconds=*)
      PEER_WAIT_SECONDS="${1#*=}"
      shift
      ;;
    --retry-count)
      RETRY_COUNT="${2:-}"
      shift 2
      ;;
    --retry-count=*)
      RETRY_COUNT="${1#*=}"
      shift
      ;;
    --keep-seeding-seconds)
      KEEP_SEEDING_SECONDS="${2:-}"
      shift 2
      ;;
    --keep-seeding-seconds=*)
      KEEP_SEEDING_SECONDS="${1#*=}"
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

if [[ -z "$SIZE" || ! "$SIZE" =~ ^[0-9]+$ || "$SIZE" -lt 1 ]]; then
  usage
  exit 2
fi

sed \
  -e "s#__ROOT_DIR__#${ROOT_DIR}#g" \
  -e "s#__SIZE__#${SIZE}#g" \
  -e "s#__SHARED_DIR__#${SHARED_DIR}#g" \
  -e "s#__RUNNER_IMAGE__#${RUNNER_IMAGE}#g" \
  -e "s#__TRACKER_IMAGE__#${TRACKER_IMAGE}#g" \
  -e "s#__WEB_SEED_URL__#${WEB_SEED_URL}#g" \
  -e "s#__PEER_WAIT_SECONDS__#${PEER_WAIT_SECONDS}#g" \
  -e "s#__RETRY_COUNT__#${RETRY_COUNT}#g" \
  -e "s#__KEEP_SEEDING_SECONDS__#${KEEP_SEEDING_SECONDS}#g" \
  "$ROOT_DIR/docker/docker-compose.template.yml"
