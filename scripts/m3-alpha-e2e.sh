#!/usr/bin/env bash
set -euo pipefail

DEFAULT_SERIALS="192.168.31.239:41351,bf9ec82f"
PACKAGE_NAME="${PACKAGE_NAME:-com.deviceagent}"
REMOTE_DIR="${REMOTE_DIR:-/sdcard/Download/m3-alpha}"
HEADLESS_INSTALL_DIR="${HEADLESS_INSTALL_DIR:-/data/local/tmp/m3-alpha}"
APP_TORRENT_DIR="${APP_TORRENT_DIR:-files/p2p}"
RESULT_ROOT="${RESULT_ROOT:-./logs/m3-alpha-s4}"
DATABASE_URL="${DATABASE_URL:-postgres://deviceops:deviceops123@localhost:5432/deviceops?sslmode=disable}"
SERVER_URL="${SERVER_URL:-http://192.168.31.81:8080}"
GRPC_PORT="${GRPC_PORT:-9090}"
WAIT_SECONDS="${E2E_WAIT_SECONDS:-${WAIT_SECONDS:-600}}"
POLL_SECONDS="${POLL_SECONDS:-5}"
HEADLESS_PM_INSTALL="${HEADLESS_PM_INSTALL:-1}"

usage() {
  cat <<'USAGE'
Usage:
  bash scripts/m3-alpha-e2e.sh [--devices serial1,serial2] [--device-ids id1,id2] [--device-count N]

Required input comes from /tmp/m3-alpha-s4/latest.env, produced by:
  cd /Users/qf/Alcedo/code/terminal-agent-dev && bash scripts/gen-torrent.sh <file_id>

Environment:
  TORRENT_PATH=<path>          Override torrent path.
  FILE_ID=<id>                 Release file id.
  SHA256=<hex>                 Expected payload SHA-256.
  FILE_SIZE=<bytes>            Optional expected file size.
  FILE_TYPE=<type>             Payload file_type. Defaults to apk; use file for download+SHA regression.
  WEB_SEED_URL=<url>           HTTP fallback URL for baseline/download evidence.
  GRPC_PORT=<port>             gRPC CommandStream port. Defaults to 9090.
  DEVICE_SERIALS=a,b           Defaults to the two known LAN devices.
  DEVICE_IDS=id1,id2           Server-side device ids. If omitted, script tries app prefs.
  RESULT_ROOT=<path>           Defaults to ./logs/m3-alpha-s4.
  APP_TORRENT_DIR=<rel-path>   App-internal torrent staging dir via run-as. Defaults to files/p2p.
  E2E_WAIT_SECONDS=<seconds>   Download evidence wait window. Defaults to 600.
  HEADLESS_PM_INSTALL=0        Disable adb pm install helper after APK P2P completion.
  SKIP_DB_TRIGGER=1            Push torrent and collect logs without inserting commands.
USAGE
}

die() {
  echo "ERROR: $*" >&2
  exit 1
}

log() {
  printf '[%s] %s\n' "$(date '+%H:%M:%S')" "$*"
}

csv_item() {
  local csv="$1"
  local index="$2"
  local old_ifs="$IFS"
  IFS=',' read -r -a items <<<"$csv"
  IFS="$old_ifs"
  printf "%s" "${items[$index]:-}"
}

csv_count() {
  local csv="$1"
  local old_ifs="$IFS"
  IFS=',' read -r -a items <<<"$csv"
  IFS="$old_ifs"
  printf "%s" "${#items[@]}"
}

sql_escape() {
  printf "%s" "$1" | sed "s/'/''/g"
}

json_escape() {
  printf "%s" "$1" | sed 's/\\/\\\\/g; s/"/\\"/g'
}

adb_shell() {
  local serial="$1"
  shift
  adb -s "$serial" shell "$@"
}

stage_torrent_for_device() {
  local serial="$1"
  local target_name
  target_name="$(basename "$TORRENT_PATH")"
  local tmp_path="/data/local/tmp/${target_name}"
  local internal_rel="${APP_TORRENT_DIR%/}/${target_name}"
  local internal_abs="/data/user/0/${PACKAGE_NAME}/${internal_rel}"

  adb -s "$serial" push "$TORRENT_PATH" "$tmp_path" >/dev/null || return 1
  adb_shell "$serial" run-as "$PACKAGE_NAME" mkdir -p "${APP_TORRENT_DIR%/}" >/dev/null 2>&1 || return 1
  adb_shell "$serial" run-as "$PACKAGE_NAME" cp "$tmp_path" "$internal_rel" >/dev/null 2>&1 || return 1
  adb_shell "$serial" run-as "$PACKAGE_NAME" chmod 666 "$internal_rel" >/dev/null 2>&1 || true
  printf "%s" "$internal_abs"
}

cleanup_p2p_artifacts() {
  local serial="$1"
  local payload_name=""
  if [[ -n "${SOURCE_FILE:-}" ]]; then
    payload_name="$(basename "$SOURCE_FILE")"
  fi
  adb_shell "$serial" run-as "$PACKAGE_NAME" rm -f \
    "${APP_TORRENT_DIR%/}/${FILE_ID}.dmg" \
    "${APP_TORRENT_DIR%/}/${FILE_ID}.dmg.part" \
    "${APP_TORRENT_DIR%/}/${FILE_ID}.apk" \
    "${APP_TORRENT_DIR%/}/${FILE_ID}.apk.part" \
    ${payload_name:+"${APP_TORRENT_DIR%/}/${payload_name}"} \
    ${payload_name:+"${APP_TORRENT_DIR%/}/${payload_name}.part"} \
    "${APP_TORRENT_DIR%/}"/*.fastresume >/dev/null 2>&1 || true
}

wait_for_device_sha() {
  local serial="$1" log_file="$2" sha_file="$3" timeout="$4"
  local elapsed=0 downloaded_path="" actual_sha=""
  local fallback_path=""

  if [[ -n "${SOURCE_FILE:-}" ]]; then
    fallback_path="/data/user/0/${PACKAGE_NAME}/${APP_TORRENT_DIR%/}/$(basename "$SOURCE_FILE")"
  fi

  while (( elapsed <= timeout )); do
    adb -s "$serial" logcat -d -v time >"$log_file" 2>&1 || true
    downloaded_path="$(LC_ALL=C sed -n 's/.*onP2PStarted:.*path=\([^ ]*\).*/\1/p' "$log_file" | tail -n 1)"
    if [[ -z "$downloaded_path" && -n "$fallback_path" ]]; then
      downloaded_path="$fallback_path"
    fi

    if [[ -n "$downloaded_path" ]]; then
      if [[ "$downloaded_path" == /data/data/"$PACKAGE_NAME"/* || "$downloaded_path" == /data/user/*/"$PACKAGE_NAME"/* ]]; then
        local app_rel="${downloaded_path#*/${PACKAGE_NAME}/}"
        actual_sha="$(adb -s "$serial" shell run-as "$PACKAGE_NAME" sha256sum "$app_rel" 2>/dev/null | awk '{print $1}' | tr -d '\r' || true)"
      else
        actual_sha="$(adb -s "$serial" shell sha256sum "$downloaded_path" 2>/dev/null | awk '{print $1}' | tr -d '\r' || true)"
      fi
      printf 'path=%s\nexpected=%s\nactual=%s\nelapsed=%s\n' "$downloaded_path" "$SHA256" "$actual_sha" "$elapsed" >"$sha_file"
      [[ -n "${SHA256:-}" && "$actual_sha" == "$SHA256" ]] && return 0
    else
      printf 'path=\nexpected=%s\nactual=\nelapsed=%s\n' "$SHA256" "$elapsed" >"$sha_file"
    fi

    if [[ -z "${SHA256:-}" ]] &&
       grep -q "SHA256.*PASS\|sha256.*ok\|verify.*success\|onP2PComplete.*success=true\|P2P download verified\|Download OK" "$log_file" 2>/dev/null; then
      return 0
    fi

    (( elapsed == timeout )) && break
    sleep "$POLL_SECONDS"
    elapsed=$((elapsed + POLL_SECONDS))
  done

  return 1
}

xml_escape() {
  printf "%s" "$1" | sed 's/&/\&amp;/g; s/</\&lt;/g; s/>/\&gt;/g; s/"/\&quot;/g'
}

read_device_id_from_prefs() {
  local serial="$1"
  local xml
  xml="$(adb -s "$serial" shell run-as "$PACKAGE_NAME" cat shared_prefs/device_agent_config.xml 2>/dev/null | tr -d '\r' || true)"
  printf "%s" "$xml" | sed -n 's/.*name="device_id">\([^<]*\)<.*/\1/p' | head -n 1
}

configure_device_agent() {
  local serial="$1"
  local device_id="$2"
  local config_file="$3"
  cat >"$config_file" <<EOF_CONFIG
<?xml version='1.0' encoding='utf-8' standalone='yes' ?>
<map>
  <string name="server_url">$(xml_escape "$SERVER_URL")</string>
  <string name="device_id">$(xml_escape "$device_id")</string>
  <string name="grpc_port">$(xml_escape "$GRPC_PORT")</string>
</map>
EOF_CONFIG
  adb_shell "$serial" am force-stop "$PACKAGE_NAME" >/dev/null 2>&1 || true
  adb -s "$serial" push "$config_file" /data/local/tmp/device_agent_config.xml >/dev/null
  adb_shell "$serial" run-as "$PACKAGE_NAME" mkdir -p shared_prefs >/dev/null 2>&1 || return 1
  adb_shell "$serial" run-as "$PACKAGE_NAME" cp /data/local/tmp/device_agent_config.xml shared_prefs/device_agent_config.xml >/dev/null 2>&1 || return 1
}

launch_device_agent() {
  local serial="$1"
  adb_shell "$serial" monkey -p "$PACKAGE_NAME" -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1 ||
    adb_shell "$serial" am start -n "${PACKAGE_NAME}/.MainActivity" >/dev/null 2>&1
}

headless_pm_install_apk() {
  local serial="$1"
  local log_file="$2"
  local out_file="$3"
  [[ "$HEADLESS_PM_INSTALL" == "1" ]] || {
    echo "HEADLESS_PM_INSTALL disabled" >"$out_file"
    return 0
  }

  local apk_path
  apk_path="$(sed -n 's/.*onP2PStarted:.*type=apk path=\([^ ]*\).*/\1/p' "$log_file" | tail -n 1)"
  if [[ -z "$apk_path" && -n "${SOURCE_FILE:-}" ]]; then
    apk_path="/data/user/0/${PACKAGE_NAME}/${APP_TORRENT_DIR%/}/$(basename "$SOURCE_FILE")"
  fi
  if [[ -z "$apk_path" ]]; then
    echo "SKIP: no P2P APK path found in logcat" >"$out_file"
    return 2
  fi

  local staged="${HEADLESS_INSTALL_DIR}/headless-$(basename "$apk_path")"
  {
    echo "source=${apk_path}"
    echo "staged=${staged}"
    adb_shell "$serial" mkdir -p "$HEADLESS_INSTALL_DIR"
    if [[ "$apk_path" == /data/data/"$PACKAGE_NAME"/* || "$apk_path" == /data/user/*/"$PACKAGE_NAME"/* ]]; then
      local host_apk="${TMPDIR:-/tmp}/m3-alpha-headless-${serial//[^A-Za-z0-9]/_}.apk"
      adb -s "$serial" exec-out run-as "$PACKAGE_NAME" cat "$apk_path" >"$host_apk"
      adb -s "$serial" push "$host_apk" "$staged"
      rm -f "$host_apk"
    else
      adb_shell "$serial" cp "$apk_path" "$staged"
    fi
    adb_shell "$serial" chmod 644 "$staged"
    adb_shell "$serial" pm install -r -d "$staged"
  } >"$out_file" 2>&1
}

detect_test_binary() {
  local serial="$1"
  local abi
  abi="$(adb -s "$serial" shell getprop ro.product.cpu.abi 2>/dev/null | tr -d '\r' || true)"
  case "$abi" in
    arm64-v8a*) printf "%s" "build-android-arm64-s3/bin/p2p_download_manager_test" ;;
    armeabi-v7a*) printf "%s" "build-android-armeabi-s3/bin/p2p_download_manager_test" ;;
    *) printf "%s" "" ;;
  esac
}

detect_cxx_shared() {
  local serial="$1"
  local abi
  abi="$(adb -s "$serial" shell getprop ro.product.cpu.abi 2>/dev/null | tr -d '\r' || true)"
  case "$abi" in
    arm64-v8a*) printf "%s" "android/app/build/intermediates/merged_native_libs/normalDebug/out/lib/arm64-v8a/libc++_shared.so" ;;
    armeabi-v7a*) printf "%s" "android/app/build/intermediates/merged_native_libs/normalDebug/out/lib/armeabi-v7a/libc++_shared.so" ;;
    *) printf "%s" "" ;;
  esac
}

run_native_test() {
  local serial="$1"
  local out_file="$2"
  local bin_path
  bin_path="$(detect_test_binary "$serial")"
  if [[ -z "$bin_path" || ! -f "$bin_path" ]]; then
    echo "SKIP: no p2p_download_manager_test binary for $(adb -s "$serial" shell getprop ro.product.cpu.abi 2>/dev/null | tr -d '\r')" | tee "$out_file"
    return 2
  fi
  local cxx_shared
  cxx_shared="$(detect_cxx_shared "$serial")"
  if [[ -z "$cxx_shared" || ! -f "$cxx_shared" ]]; then
    echo "SKIP: no libc++_shared.so for $(adb -s "$serial" shell getprop ro.product.cpu.abi 2>/dev/null | tr -d '\r')" | tee "$out_file"
    return 2
  fi
  adb -s "$serial" push "$bin_path" /data/local/tmp/p2p_download_manager_test >>"$out_file" 2>&1
  adb -s "$serial" push "$cxx_shared" /data/local/tmp/libc++_shared.so >>"$out_file" 2>&1
  adb_shell "$serial" chmod 755 /data/local/tmp/p2p_download_manager_test >>"$out_file" 2>&1
  adb_shell "$serial" "LD_LIBRARY_PATH=/data/local/tmp TMPDIR=/data/local/tmp /data/local/tmp/p2p_download_manager_test" >>"$out_file" 2>&1
}

insert_download_ready() {
  local device_id="$1"
  local batch_id="$2"
  local command_id="$3"
  local command_row_id="$4"
  local remote_torrent="$5"
  local payload
  payload=$(printf '{"batch_id":"%s","file_id":"%s","file_type":"%s","download_url":"%s","sha256":"%s","file_size":%s,"torrent_url":"%s"}' \
    "$(json_escape "$batch_id")" \
    "$(json_escape "$FILE_ID")" \
    "$(json_escape "$FILE_TYPE")" \
    "$(json_escape "${WEB_SEED_URL:-}")" \
    "$(json_escape "${SHA256:-}")" \
    "${FILE_SIZE:-0}" \
    "$(json_escape "$remote_torrent")")

  psql "$DATABASE_URL" -v ON_ERROR_STOP=1 -c "
INSERT INTO commands (id, command_id, device_id, command_type, payload_json, status, timeout_seconds, issued_at, created_by)
VALUES ('$(sql_escape "$command_row_id")', '$(sql_escape "$command_id")', '$(sql_escape "$device_id")',
        'download_ready', '$(sql_escape "$payload")', 'pending', 3600, NOW(), 'm3-alpha-e2e');
" >/dev/null
}

ensure_release_fixture() {
  local device_id="$1" batch_id="$2" task_id="$3" serial="$4"
  psql "$DATABASE_URL" -v ON_ERROR_STOP=1 -c "
INSERT INTO devices (id, name, type, region, address, status, last_heartbeat, firmware, config, installed_at, stats, capabilities)
VALUES ('$(sql_escape "$device_id")', 'm3-alpha-e2e $(sql_escape "$device_id")', 'android', 'lab',
        '$(sql_escape "$serial")', 'online', NOW(), 'v1.0.0', '{}'::jsonb, NOW(), '{}'::jsonb, '{}'::jsonb)
ON CONFLICT (id) DO UPDATE SET status = 'online', address = EXCLUDED.address, last_heartbeat = NOW();

INSERT INTO files (file_id, file_name, file_size, file_type, sha256, storage_path, storage_mode, upload_mode, created_by)
VALUES ('$(sql_escape "$FILE_ID")', '$(sql_escape "$FILE_ID").$( [[ "$FILE_TYPE" == "apk" ]] && echo apk || echo dmg )',
        ${FILE_SIZE:-0}, '$(sql_escape "$FILE_TYPE")',
        '$(sql_escape "$SHA256")', '$(sql_escape "${SOURCE_FILE:-$TORRENT_PATH}")', 'local', 'p2p', 'm3-alpha-e2e')
ON CONFLICT (file_id) DO UPDATE SET file_size = EXCLUDED.file_size,
    file_type = EXCLUDED.file_type, sha256 = EXCLUDED.sha256, storage_path = EXCLUDED.storage_path;

INSERT INTO release_tasks (task_id, name, file_id, file_type, strategy, total_batches, batch_size,
                           status, created_by, total_devices, pending_devices)
VALUES ('$(sql_escape "$task_id")', 'm3-alpha-e2e-${RUN_ID}', '$(sql_escape "$FILE_ID")',
        '$(sql_escape "$FILE_TYPE")', 'manual', 1, 1, 'running', 'm3-alpha-e2e', 1, 1)
ON CONFLICT (task_id) DO UPDATE SET status = 'running', updated_at = NOW();

INSERT INTO release_batches (task_id, batch_id, batch_number, status, strategy, total_devices,
                             pending_devices, downloading_devices, succeeded_devices, failed_devices, started_at)
VALUES ('$(sql_escape "$task_id")', '$(sql_escape "$batch_id")', 1, 'running', 'manual',
        1, 1, 0, 0, 0, NOW())
ON CONFLICT (batch_id) DO UPDATE SET status = 'running', pending_devices = 1,
    downloading_devices = 0, succeeded_devices = 0, failed_devices = 0, updated_at = NOW();

INSERT INTO release_batch_devices (batch_id, device_id, file_id, status, downloaded_bytes, ready_at, updated_at)
VALUES ('$(sql_escape "$batch_id")', '$(sql_escape "$device_id")', '$(sql_escape "$FILE_ID")',
        'pending', 0, NOW(), NOW())
ON CONFLICT (batch_id, device_id) DO UPDATE SET status = 'pending', downloaded_bytes = 0,
    error_code = NULL, error_message = NULL, ready_at = NOW(), downloading_at = NULL,
    downloaded_at = NULL, installing_at = NULL, installed_at = NULL, failed_at = NULL,
    completion_path = NULL, updated_at = NOW();
" >/dev/null
}

collect_command_rows() {
  local result_dir="$1"
  if command -v psql >/dev/null 2>&1; then
    psql "$DATABASE_URL" -At -F $'\t' -c "SELECT device_id, command_id, status, COALESCE(result_message, '') FROM commands WHERE created_by = 'm3-alpha-e2e' AND id LIKE 'CMD-${RUN_ID}-%' ORDER BY device_id" \
      >"${result_dir}/commands.tsv" 2>"${result_dir}/commands.err" || true
  fi
}

write_summary() {
  local result_dir="$1"
  local summary="$result_dir/summary.md"
  local peer_positive="NO"
  local stats_present="NO"
  local torrent_pushed_result="FAIL"
  local sha_result="FAIL"
  local native_result="FAIL"
  local seeding_result="FAIL"
  local regression_verdict="FAIL"
  local complete_count
  local native_pass_count
  local sha_pass_count
  local web_seed_bytes
  local peer_bytes

  peer_bytes="$(grep -RhoE 'from_peers=[0-9]+' "$result_dir"/logcat-*.log 2>/dev/null | awk -F= '{sum += $2} END {print sum + 0}')"
  web_seed_bytes="$(grep -RhoE 'from_web_seed=[0-9]+' "$result_dir"/logcat-*.log 2>/dev/null | awk -F= '{sum += $2} END {print sum + 0}')"
  if (( peer_bytes > 0 || web_seed_bytes > 0 )); then stats_present="YES"; fi
  if (( peer_bytes > 0 )); then peer_positive="YES"; fi
  complete_count="$(grep -Rhc 'onP2PComplete.*success=true\|Install SUCCESS\|downloaded' "$result_dir"/logcat-*.log 2>/dev/null | awk '{sum += $1} END {print sum + 0}')"
  native_pass_count="$(grep -Rhc '^native_test=PASS' "$result_dir"/device-*.env 2>/dev/null | awk '{sum += $1} END {print sum + 0}')"
  sha_pass_count="$(grep -Rhc '^status=PASS$' "$result_dir"/sha256-*.txt 2>/dev/null | awk '{sum += $1} END {print sum + 0}')"
  [[ -s "$TORRENT_PATH" ]] && torrent_pushed_result="PASS"
  (( sha_pass_count >= DEVICE_COUNT )) && sha_result="PASS"
  (( sha_pass_count > 0 && sha_pass_count < DEVICE_COUNT )) && sha_result="PARTIAL"
  (( native_pass_count > 0 )) && native_result="PASS"
  grep -Rqh 'Seeding\|onP2PComplete.*success=true' "$result_dir"/logcat-*.log 2>/dev/null && seeding_result="PARTIAL"
  if [[ "$peer_positive" == YES && "$sha_result" == PASS ]]; then
    regression_verdict="PASS"
  fi

  cat >"$summary" <<EOF_SUMMARY
# M3-A S4 LAN E2E Result

- Run ID: \`${RUN_ID}\`
- Generated: \`$(date -u +%Y-%m-%dT%H:%M:%SZ)\`
- File ID: \`${FILE_ID:-}\`
- Torrent: \`${TORRENT_PATH:-}\`
- Web seed: \`${WEB_SEED_URL:-}\`
- Devices requested: \`${DEVICE_COUNT}\`
- Device serials: \`${DEVICE_SERIALS}\`
- Device ids: \`${DEVICE_IDS_RESOLVED}\`
- Wait seconds: \`${WAIT_SECONDS}\`

## Evidence

- Native C++ test PASS count: \`${native_pass_count}/${DEVICE_COUNT}\`
- P2P complete/install log hits: \`${complete_count}\`
- SHA-256 PASS count: \`${sha_pass_count}/${DEVICE_COUNT}\`
- Parsed \`from_peers\` bytes: \`${peer_bytes}\`
- Parsed \`from_web_seed\` bytes: \`${web_seed_bytes}\`
- Commands: \`commands.tsv\`
- Per-device logs: \`logcat-*.log\`, \`native-test-*.log\`, \`device-*.env\`

## Regression Verdict

${regression_verdict}

## AC Gate

| AC | Result | Evidence |
| --- | --- | --- |
| AC1 torrent exists and pushed | ${torrent_pushed_result} | \`${TORRENT_PATH}\` |
| AC2 at least one device got peer pieces | $([[ "$peer_positive" == YES ]] && echo PASS || echo FAIL) | parsed \`from_peers=${peer_bytes}\` |
| AC3 SHA256 verified on device | ${sha_result} | \`sha256-*.txt\` expected vs actual |
| AC4 bandwidth comparison data | $([[ "$stats_present" == YES ]] && echo PASS || echo FAIL) | parsed peer/web-seed bytes |
| AC5 seeding lifecycle observed | ${seeding_result} | logcat lifecycle markers |
| RV-20260523-02 native test executed | ${native_result} | native-test logs |

## Notes

- This script intentionally fails AC2/AC4 if device logs do not expose \`from_peers\` / \`from_web_seed\` counters.
- S4 scope is script-only; if counters are absent in this run, review should decide whether to open a follow-up for native stats logging.
EOF_SUMMARY

  echo "$summary"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --devices) DEVICE_SERIALS="$2"; shift 2 ;;
    --device-ids) DEVICE_IDS="${2:-}"; shift 2 ;;
    --device-count) DEVICE_COUNT="${2:-}"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) die "unknown argument: $1" ;;
  esac
done

PRESET_TORRENT_PATH="${TORRENT_PATH:-}"
PRESET_FILE_ID="${FILE_ID:-}"
PRESET_SHA256="${SHA256:-}"
PRESET_FILE_SIZE="${FILE_SIZE:-}"
PRESET_FILE_TYPE="${FILE_TYPE:-}"
PRESET_WEB_SEED_URL="${WEB_SEED_URL:-}"
PRESET_SOURCE_FILE="${SOURCE_FILE:-}"
if [[ -f "${M3_ALPHA_ENV:-/tmp/m3-alpha-s4/latest.env}" ]]; then
  # shellcheck disable=SC1090
  source "${M3_ALPHA_ENV:-/tmp/m3-alpha-s4/latest.env}"
fi
TORRENT_PATH="${PRESET_TORRENT_PATH:-${TORRENT_PATH:-}}"
FILE_ID="${PRESET_FILE_ID:-${FILE_ID:-}}"
SHA256="${PRESET_SHA256:-${SHA256:-}}"
FILE_SIZE="${PRESET_FILE_SIZE:-${FILE_SIZE:-0}}"
FILE_TYPE="${PRESET_FILE_TYPE:-${FILE_TYPE:-apk}}"
WEB_SEED_URL="${PRESET_WEB_SEED_URL:-${WEB_SEED_URL:-}}"
SOURCE_FILE="${PRESET_SOURCE_FILE:-${SOURCE_FILE:-}}"

DEVICE_SERIALS="${DEVICE_SERIALS:-$DEFAULT_SERIALS}"
DEVICE_COUNT="${DEVICE_COUNT:-$(csv_count "$DEVICE_SERIALS")}"
WEB_SEED_URL="${WEB_SEED_URL:-${SERVER_URL%/}/api/v1/files/${FILE_ID}/download}"

[[ -n "$TORRENT_PATH" ]] || die "TORRENT_PATH missing; run terminal-agent-dev/scripts/gen-torrent.sh <file_id> first"
[[ -f "$TORRENT_PATH" ]] || die "torrent not found: ${TORRENT_PATH}"
[[ -n "$FILE_ID" ]] || die "FILE_ID missing"
command -v adb >/dev/null 2>&1 || die "adb not found"

if [[ "${SKIP_DB_TRIGGER:-0}" != "1" ]]; then
  command -v psql >/dev/null 2>&1 || die "psql not found; set SKIP_DB_TRIGGER=1 to push/capture only"
fi

RUN_ID="${RUN_ID:-$(date +%Y%m%d-%H%M%S)}"
result_dir="${RESULT_ROOT}/${RUN_ID}"
mkdir -p "$result_dir"

log "result_dir=${result_dir}"
log "torrent=${TORRENT_PATH}"

DEVICE_IDS_RESOLVED=""
logcat_pids=()

cleanup() {
  for pid in "${logcat_pids[@]:-}"; do
    kill "$pid" >/dev/null 2>&1 || true
  done
}
trap cleanup EXIT

for ((i=0; i<DEVICE_COUNT; i++)); do
  serial="$(csv_item "$DEVICE_SERIALS" "$i")"
  [[ -n "$serial" ]] || die "missing serial at index $i"
  log "adb connect ${serial}"
  adb connect "$serial" | tee "${result_dir}/adb-connect-${i}.log"

  device_id="$(csv_item "${DEVICE_IDS:-}" "$i")"
  if [[ -z "$device_id" ]]; then
    device_id="$(read_device_id_from_prefs "$serial")"
  fi
  if [[ -z "$device_id" ]]; then
    device_id="ANDROID-$((i + 1))"
    log "WARN: device_id for ${serial} not found from prefs; fallback ${device_id}. Override with DEVICE_IDS=id1,id2 if needed."
  fi
  DEVICE_IDS_RESOLVED="${DEVICE_IDS_RESOLVED}${DEVICE_IDS_RESOLVED:+,}${device_id}"
  if configure_device_agent "$serial" "$device_id" "${result_dir}/config-${i}.xml"; then
    log "configured ${serial} as ${device_id}"
  else
    log "WARN: unable to write app prefs via run-as for ${serial}; assuming device is already configured as ${device_id}"
  fi

  cleanup_p2p_artifacts "$serial"
  remote_torrent="$(stage_torrent_for_device "$serial")" || die "failed to stage torrent into app internal dir on ${serial}"
  printf 'staged_torrent=%s\n' "$remote_torrent" | tee "${result_dir}/adb-push-${i}.log"

  native_status="SKIP"
  if run_native_test "$serial" "${result_dir}/native-test-${i}.log"; then
    native_status="PASS"
  else
    native_status="FAIL_OR_SKIP"
  fi

  adb -s "$serial" logcat -c || true
  adb -s "$serial" logcat -v time >"${result_dir}/logcat-${i}.log" 2>&1 &
  logcat_pids+=("$!")

  launch_device_agent "$serial" || log "WARN: unable to launch ${PACKAGE_NAME}/.MainActivity on ${serial}"

  command_id="$(uuidgen | tr 'A-Z' 'a-z')"
  batch_id="$(uuidgen | tr 'A-Z' 'a-z')"
  task_id="$(uuidgen | tr 'A-Z' 'a-z')"
  command_row_id="CMD-${RUN_ID}-${i}"
  if [[ "${SKIP_DB_TRIGGER:-0}" != "1" ]]; then
    ensure_release_fixture "$device_id" "$batch_id" "$task_id" "$serial"
    insert_download_ready "$device_id" "$batch_id" "$command_id" "$command_row_id" "$remote_torrent"
  fi

  cat >"${result_dir}/device-${i}.env" <<EOF_DEVICE
serial=${serial}
device_id=${device_id}
remote_torrent=${remote_torrent}
command_id=${command_id}
command_row_id=${command_row_id}
task_id=${task_id}
batch_id=${batch_id}
native_test=${native_status}
EOF_DEVICE
done

log "waiting ${WAIT_SECONDS}s for P2P/download evidence"
elapsed=0
while (( elapsed < WAIT_SECONDS )); do
  sleep "$POLL_SECONDS"
  elapsed=$((elapsed + POLL_SECONDS))
  collect_command_rows "$result_dir"
done

for ((i=0; i<DEVICE_COUNT; i++)); do
  serial="$(csv_item "$DEVICE_SERIALS" "$i")"
  if wait_for_device_sha "$serial" "${result_dir}/logcat-${i}.log" "${result_dir}/sha256-${i}.txt" 1; then
    echo "status=PASS" >>"${result_dir}/sha256-${i}.txt"
  else
    echo "status=FAIL" >>"${result_dir}/sha256-${i}.txt"
  fi
done

for ((i=0; i<DEVICE_COUNT; i++)); do
  serial="$(csv_item "$DEVICE_SERIALS" "$i")"
  headless_pm_install_apk "$serial" "${result_dir}/logcat-${i}.log" "${result_dir}/headless-install-${i}.log" || true
done

for pid in "${logcat_pids[@]}"; do
  kill "$pid" >/dev/null 2>&1 || true
done
trap - EXIT

summary="$(write_summary "$result_dir")"
log "summary=${summary}"

if grep -q '| AC2 at least one device got peer pieces | PASS ' "$summary" && \
   grep -q '| AC3 SHA256 verified on device | PASS ' "$summary" && \
   grep -q '^PASS$' "$summary" && \
   grep -q '| AC4 bandwidth comparison data | PASS ' "$summary"; then
  log "S4 E2E PASS"
else
  log "S4 E2E evidence incomplete; see ${summary}"
  exit 3
fi
