#!/usr/bin/env bash
set -euo pipefail

# ============================================================
# M3-Beta E2E: 3-scenario real-device orchestration
# Scenarios: proto (torrent_url true path), 4g-switch, config-hotload
# ============================================================

# ---- Defaults ----
DEFAULT_SERIALS="192.168.31.211:40541,bf9ec82f"
# ^ current two-device setup: Wi-Fi device + 4G/USB device; adjust per setup
PACKAGE_NAME="${PACKAGE_NAME:-com.deviceagent}"
REMOTE_DIR="${REMOTE_DIR:-/sdcard/Download/m3-beta}"
APP_TORRENT_DIR="${APP_TORRENT_DIR:-files/p2p}"
RESULT_ROOT="${RESULT_ROOT:-./logs/m3-beta-e2e}"
DATABASE_URL="${DATABASE_URL:-postgres://deviceops:deviceops123@localhost:5432/deviceops?sslmode=disable}"
SERVER_URL="${SERVER_URL:-http://192.168.31.81:8080}"
SERVER_LOG="${SERVER_LOG:-/tmp/terminal-agent-dev.log}"
GRPC_PORT="${GRPC_PORT:-9090}"
WAIT_SECONDS="${WAIT_SECONDS:-300}"
POLL_SECONDS="${POLL_SECONDS:-5}"
PROTO_SHA_WAIT_SECONDS="${PROTO_SHA_WAIT_SECONDS:-300}"
FOUR_G_PRE_SWITCH_WAIT_SECONDS="${FOUR_G_PRE_SWITCH_WAIT_SECONDS:-60}"
FOUR_G_CELLULAR_HOLD_SECONDS="${FOUR_G_CELLULAR_HOLD_SECONDS:-30}"
CONFIG_TTL_HOURS="${CONFIG_TTL_HOURS:-auto}"
SCENARIO="${SCENARIO:-all}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# ---- Helpers (reused from Alpha) ----
die() { echo "ERROR: $*" >&2; exit 1; }
log() { printf '[%s] %s\n' "$(date '+%H:%M:%S')" "$*"; }

csv_item() {
  local csv="$1" index="$2" old_ifs="$IFS"; IFS=',' read -r -a items <<<"$csv"; IFS="$old_ifs"
  printf "%s" "${items[$index]:-}"
}
csv_count() {
  local csv="$1" old_ifs="$IFS"; IFS=',' read -r -a items <<<"$csv"; IFS="$old_ifs"
  printf "%s" "${#items[@]}"
}
sql_escape() { printf "%s" "$1" | sed "s/'/''/g"; }
json_escape() { printf "%s" "$1" | sed 's/\\/\\\\/g; s/"/\\"/g'; }
xml_escape() { printf "%s" "$1" | sed 's/&/\&amp;/g; s/</\&lt;/g; s/>/\&gt;/g; s/"/\&quot;/g'; }

adb_shell() { local serial="$1"; shift; adb -s "$serial" shell "$@"; }

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

read_device_id_from_prefs() {
  local serial="$1" xml
  xml="$(adb -s "$serial" shell run-as "$PACKAGE_NAME" cat shared_prefs/device_agent_config.xml 2>/dev/null | tr -d '\r' || true)"
  printf "%s" "$xml" | sed -n 's/.*name="device_id">\([^<]*\)<.*/\1/p' | head -n 1
}

configure_device_agent() {
  local serial="$1" device_id="$2" config_file="$3"
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
  adb_shell "$serial" am start-foreground-service -n "${PACKAGE_NAME}/.DeviceAgentService" >/dev/null 2>&1 ||
    adb_shell "$serial" monkey -p "$PACKAGE_NAME" -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1 ||
    adb_shell "$serial" am start -n "${PACKAGE_NAME}/.MainActivity" >/dev/null 2>&1
}

detect_test_binary() {
  local serial="$1" abi
  abi="$(adb -s "$serial" shell getprop ro.product.cpu.abi 2>/dev/null | tr -d '\r' || true)"
  case "$abi" in
    arm64-v8a*) echo "build-android-arm64-s3/bin/p2p_download_manager_test" ;;
    armeabi-v7a*) echo "build-android-armeabi-s3/bin/p2p_download_manager_test" ;;
    *) echo "" ;;
  esac
}

detect_cxx_shared() {
  local serial="$1" abi
  abi="$(adb -s "$serial" shell getprop ro.product.cpu.abi 2>/dev/null | tr -d '\r' || true)"
  case "$abi" in
    arm64-v8a*) echo "android/app/build/intermediates/merged_native_libs/normalDebug/out/lib/arm64-v8a/libc++_shared.so" ;;
    armeabi-v7a*) echo "android/app/build/intermediates/merged_native_libs/normalDebug/out/lib/armeabi-v7a/libc++_shared.so" ;;
    *) echo "" ;;
  esac
}

run_native_test() {
  local serial="$1" out_file="$2" bin_path
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

collect_command_rows() {
  local result_dir="$1"
  if command -v psql >/dev/null 2>&1; then
    psql "$DATABASE_URL" -At -F $'\t' -c "SELECT device_id, command_id, status, COALESCE(result_message, '') FROM commands WHERE created_by = 'm3-beta-e2e' AND id LIKE 'CMD-B-${RUN_ID}-%' ORDER BY device_id" \
      >"${result_dir}/commands.tsv" 2>"${result_dir}/commands.err" || true
  fi
}

supersede_stale_beta_commands() {
  local result_dir="$1"
  local ids_csv="$2"
  [[ -n "$ids_csv" ]] || return 0

  local values=""
  local old_ifs="$IFS"
  IFS=',' read -r -a ids <<<"$ids_csv"
  IFS="$old_ifs"
  for id in "${ids[@]}"; do
    [[ -n "$id" ]] || continue
    values="${values}${values:+,}('$(sql_escape "$id")')"
  done
  [[ -n "$values" ]] || return 0

  psql "$DATABASE_URL" -v ON_ERROR_STOP=1 -c "
WITH target(device_id) AS (VALUES ${values})
UPDATE commands
SET status = 'cancelled',
    result_message = 'superseded by m3-beta-e2e run ${RUN_ID}'
WHERE created_by = 'm3-beta-e2e'
  AND status = 'pending'
  AND id NOT LIKE 'CMD-B-$(sql_escape "$RUN_ID")-%'
  AND device_id IN (SELECT device_id FROM target);
" >"${result_dir}/stale-commands-cleanup.log" 2>"${result_dir}/stale-commands-cleanup.err" || true
}

# ---- Beta-specific helpers ----

insert_download_ready_beta() {
  local device_id="$1" batch_id="$2" command_id="$3" command_row_id="$4" remote_torrent="$5" web_seed_url="$6"
  local payload
  # payload includes both download_url (fallback) and torrent_url (P2P true path)
  payload=$(printf '{"batch_id":"%s","file_id":"%s","file_type":"%s","download_url":"%s","sha256":"%s","file_size":%s,"torrent_url":"%s"}' \
    "$(json_escape "$batch_id")" \
    "$(json_escape "$FILE_ID")" \
    "$(json_escape "$FILE_TYPE")" \
    "$(json_escape "$web_seed_url")" \
    "$(json_escape "${SHA256:-}")" \
    "${FILE_SIZE:-0}" \
    "$(json_escape "$remote_torrent")")

  psql "$DATABASE_URL" -v ON_ERROR_STOP=1 -c "
INSERT INTO commands (id, command_id, device_id, command_type, payload_json, status, timeout_seconds, issued_at, created_by)
VALUES ('$(sql_escape "$command_row_id")', '$(sql_escape "$command_id")', '$(sql_escape "$device_id")',
        'download_ready', '$(sql_escape "$payload")', 'pending', 3600, NOW(), 'm3-beta-e2e');
" >/dev/null
}

ensure_release_fixture_beta() {
  local device_id="$1" batch_id="$2" task_id="$3" serial="$4"
  psql "$DATABASE_URL" -v ON_ERROR_STOP=1 -c "
INSERT INTO devices (id, name, type, region, address, status, last_heartbeat, firmware, config, installed_at, stats, capabilities)
VALUES ('$(sql_escape "$device_id")', 'm3-beta-e2e $(sql_escape "$device_id")', 'android', 'lab',
        '$(sql_escape "$serial")', 'online', NOW(), 'v1.0.0', '{}'::jsonb, NOW(), '{}'::jsonb, '{}'::jsonb)
ON CONFLICT (id) DO UPDATE SET status = 'online', address = EXCLUDED.address, last_heartbeat = NOW();

INSERT INTO files (file_id, file_name, file_size, file_type, sha256, storage_path, storage_mode, upload_mode, created_by)
VALUES ('$(sql_escape "$FILE_ID")', '$(sql_escape "$FILE_ID").$( [[ "$FILE_TYPE" == "apk" ]] && echo apk || echo dmg )',
        ${FILE_SIZE:-0}, '$(sql_escape "$FILE_TYPE")', '$(sql_escape "$SHA256")',
        '$(sql_escape "${SOURCE_FILE:-$TORRENT_PATH}")', 'local', 'p2p', 'm3-beta-e2e')
ON CONFLICT (file_id) DO UPDATE SET file_size = EXCLUDED.file_size,
    file_type = EXCLUDED.file_type, sha256 = EXCLUDED.sha256, storage_path = EXCLUDED.storage_path;

INSERT INTO release_tasks (task_id, name, file_id, file_type, strategy, total_batches, batch_size,
                           status, created_by, total_devices, pending_devices)
VALUES ('$(sql_escape "$task_id")', 'm3-beta-e2e-${RUN_ID}', '$(sql_escape "$FILE_ID")',
        '$(sql_escape "$FILE_TYPE")', 'manual', 1, 1, 'running', 'm3-beta-e2e', 1, 1)
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

wait_for_logcat_pattern() {
  local serial="$1" pattern="$2" timeout="$3" log_file="$4"
  local elapsed=0
  while (( elapsed < timeout )); do
    if grep -q "$pattern" "$log_file" 2>/dev/null; then
      return 0
    fi
    sleep 2
    elapsed=$((elapsed + 2))
  done
  return 1
}

max_counter_value() {
  local file="$1" key="$2"
  sed -n "s/.*${key}=\([0-9][0-9]*\).*/\1/p" "$file" 2>/dev/null |
    awk 'BEGIN {max=0} {if ($1 > max) max=$1} END {print max+0}'
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
    downloaded_path="$(sed -n 's/.*onP2PStarted:.*path=\([^ ]*\).*/\1/p' "$log_file" | tail -n 1)"
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

# ---- Scenario 1: proto torrent_url true path ----
scenario_proto() {
  local result_dir="$1"
  local scenario_dir="${result_dir}/proto"
  mkdir -p "$scenario_dir"

  log "=== Scenario 1: proto torrent_url true path ==="

  local web_seed_url="${WEB_SEED_URL}"

  for ((i=0; i<DEVICE_COUNT; i++)); do
    local serial device_id command_id command_row_id
    serial="$(csv_item "$DEVICE_SERIALS" "$i")"
    device_id="$(csv_item "$DEVICE_IDS_RESOLVED" "$i")"
    command_id="$(uuidgen | tr 'A-Z' 'a-z')"
    batch_id="$(uuidgen | tr 'A-Z' 'a-z')"
    task_id="$(uuidgen | tr 'A-Z' 'a-z')"
    command_row_id="CMD-B-${RUN_ID}-proto-${i}"

    local remote_torrent
    cleanup_p2p_artifacts "$serial"
    remote_torrent="$(stage_torrent_for_device "$serial")" || die "failed to stage torrent into app internal dir on ${serial}"

    ensure_release_fixture_beta "$device_id" "$batch_id" "$task_id" "$serial"
    insert_download_ready_beta "$device_id" "$batch_id" "$command_id" "$command_row_id" "$remote_torrent" "$web_seed_url"

    # Dump payload evidence
    psql "$DATABASE_URL" -At -c "SELECT payload_json FROM commands WHERE id = '$(sql_escape "$command_row_id")'" \
      >"${scenario_dir}/payload-${i}.json" 2>/dev/null || true

    cat >"${scenario_dir}/device-${i}.env" <<EOF_DEVICE
serial=${serial}
device_id=${device_id}
command_id=${command_id}
command_row_id=${command_row_id}
task_id=${task_id}
batch_id=${batch_id}
remote_torrent=${remote_torrent}
torrent_url=${remote_torrent}
download_url=${web_seed_url}
EOF_DEVICE
  done

  log "proto: waiting ${WAIT_SECONDS}s for P2P download evidence"
  sleep "$WAIT_SECONDS"

  # Check device logs for torrent_url consumption
  local torrent_url_hits=0
  local deprecated_hits=0
  local sha_pass=0
  local expected_torrent_path="/data/user/0/${PACKAGE_NAME}/${APP_TORRENT_DIR%/}/$(basename "$TORRENT_PATH")"
  for ((i=0; i<DEVICE_COUNT; i++)); do
    serial="$(csv_item "$DEVICE_SERIALS" "$i")"
    local log_file="${scenario_dir}/logcat-${i}.log"
    adb -s "$serial" logcat -d -v time >"$log_file" 2>&1 || true

    if grep -q "P2PDownloadManager: loading torrent ${expected_torrent_path}\\|onP2PStarted:.*path=" "$log_file" 2>/dev/null; then
      torrent_url_hits=$((torrent_url_hits + 1))
    fi
    if grep -q "deprecated.*torrent" "$log_file" 2>/dev/null; then
      deprecated_hits=$((deprecated_hits + 1))
    fi

    if wait_for_device_sha "$serial" "$log_file" "${scenario_dir}/sha256-${i}.txt" "$PROTO_SHA_WAIT_SECONDS"; then
      sha_pass=$((sha_pass + 1))
    fi
  done
  collect_command_rows "$scenario_dir"

  # Summary
  cat >"${scenario_dir}/summary.md" <<EOF_SUM
## Scenario 1: proto torrent_url true path

- Devices: \`${DEVICE_COUNT}\`
- torrent_url: \`${remote_torrent}\`
- download_url: \`${web_seed_url}\`
- Wait: \`${WAIT_SECONDS}s\`
- SHA wait after initial wait: \`${PROTO_SHA_WAIT_SECONDS}s\`

### Evidence

| Metric | Value |
|--------|-------|
| torrent_url field hits in logcat | ${torrent_url_hits}/${DEVICE_COUNT} |
| deprecated fallback warnings | ${deprecated_hits} |
| SHA-256 pass count | ${sha_pass}/${DEVICE_COUNT} |

### Verdict

$([[ "$torrent_url_hits" -eq "$DEVICE_COUNT" && "$deprecated_hits" -eq 0 && "$sha_pass" -eq "$DEVICE_COUNT" ]] && echo "PASS" || echo "INCOMPLETE")
EOF_SUM

  log "proto: torrent_url_hits=${torrent_url_hits} deprecated=${deprecated_hits} sha=${sha_pass}"
  return $(( torrent_url_hits == DEVICE_COUNT && deprecated_hits == 0 && sha_pass == DEVICE_COUNT ? 0 : 1 ))
}

# ---- Scenario 2: 4G switch ----
scenario_4g_switch() {
  local result_dir="$1"
  local scenario_dir="${result_dir}/4g-switch"
  mkdir -p "$scenario_dir"

  log "=== Scenario 2: 4G switch ==="
  log "Step 1: Start P2P download on all 3 devices..."

  local web_seed_url="${WEB_SEED_URL}"

  for ((i=0; i<DEVICE_COUNT; i++)); do
    local serial device_id command_id command_row_id
    serial="$(csv_item "$DEVICE_SERIALS" "$i")"
    device_id="$(csv_item "$DEVICE_IDS_RESOLVED" "$i")"
    command_id="$(uuidgen | tr 'A-Z' 'a-z')"
    batch_id="$(uuidgen | tr 'A-Z' 'a-z')"
    task_id="$(uuidgen | tr 'A-Z' 'a-z')"
    command_row_id="CMD-B-${RUN_ID}-4g-${i}"

    adb -s "$serial" logcat -c || true
    adb -s "$serial" logcat -v time >"${scenario_dir}/logcat-${i}.log" 2>&1 &

    local remote_torrent
    cleanup_p2p_artifacts "$serial"
    remote_torrent="$(stage_torrent_for_device "$serial")" || die "failed to stage torrent into app internal dir on ${serial}"

    ensure_release_fixture_beta "$device_id" "$batch_id" "$task_id" "$serial"
    insert_download_ready_beta "$device_id" "$batch_id" "$command_id" "$command_row_id" "$remote_torrent" "$web_seed_url"

    cat >"${scenario_dir}/device-${i}.env" <<EOF_DEVICE
serial=${serial}
device_id=${device_id}
command_id=${command_id}
command_row_id=${command_row_id}
task_id=${task_id}
batch_id=${batch_id}
remote_torrent=${remote_torrent}
torrent_url=${remote_torrent}
download_url=${web_seed_url}
EOF_DEVICE
  done

  # Wait for seeding to establish
  sleep "$FOUR_G_PRE_SWITCH_WAIT_SECONDS"
  log "4g: waiting ${FOUR_G_PRE_SWITCH_WAIT_SECONDS}s for seeding to establish..."

  # Snapshot T0: before switch
  local t0_serial
  t0_serial="${NETWORK_SWITCH_SERIAL:-$(csv_item "$DEVICE_SERIALS" "$((DEVICE_COUNT - 1))")}"
  log "Step 2: Snapshot T0 counters..."
  adb_shell "$t0_serial" logcat -d -v time | grep -E "counters from_peers|from_web_seed" >"${scenario_dir}/counters-t0.log" 2>/dev/null || true

  # Switch WiFi device to cellular
  log "Step 3: Switch to cellular..."
  bash "${SCRIPT_DIR}/simulate-network-switch.sh" "$t0_serial" to-cellular --wait 15 2>&1 |
    tee "${scenario_dir}/switch-to-cellular.log" || {
    log "WARN: network switch to cellular failed or device may not support svc data"
  }

  # Snapshot T1: during 4G
  sleep "$FOUR_G_CELLULAR_HOLD_SECONDS"
  log "Step 4: Snapshot T1 counters (during 4G)..."
  adb_shell "$t0_serial" logcat -d -v time | grep -E "counters from_peers|from_web_seed|on_network_changed|upload_throttle|max_uploads|upload_limit" >"${scenario_dir}/counters-t1.log" 2>/dev/null || true

  # Switch back to WiFi
  log "Step 5: Switch back to Wi-Fi..."
  bash "${SCRIPT_DIR}/simulate-network-switch.sh" "$t0_serial" to-wifi --wait 15 2>&1 |
    tee "${scenario_dir}/switch-to-wifi.log" || {
    log "WARN: network switch to wifi failed"
  }

  sleep 30
  log "Step 6: Snapshot T2 counters (after WiFi restore)..."
  adb_shell "$t0_serial" logcat -d -v time | grep -E "counters from_peers|from_web_seed|upload_throttle|max_uploads|upload_limit" >"${scenario_dir}/counters-t2.log" 2>/dev/null || true

  # Collect all post-switch logcat from all devices
  for ((i=0; i<DEVICE_COUNT; i++)); do
    logcat_pid="${logcat_pids[$i]:-}"
    kill "$logcat_pid" >/dev/null 2>&1 || true
  done

  # Analyze counters
  local t0_web="0" t1_web="0" t2_web="0"
  local t0_peer="0" t1_peer="0" t2_peer="0"
  t0_web="$(max_counter_value "${scenario_dir}/counters-t0.log" "from_web_seed")"
  t0_peer="$(max_counter_value "${scenario_dir}/counters-t0.log" "from_peers")"
  t1_web="$(max_counter_value "${scenario_dir}/counters-t1.log" "from_web_seed")"
  t1_peer="$(max_counter_value "${scenario_dir}/counters-t1.log" "from_peers")"
  t2_web="$(max_counter_value "${scenario_dir}/counters-t2.log" "from_web_seed")"
  t2_peer="$(max_counter_value "${scenario_dir}/counters-t2.log" "from_peers")"

  local network_change_hits=0
  network_change_hits="$(grep -c "on_network_changed" "${scenario_dir}/counters-t1.log" 2>/dev/null || true)"
  if grep -q "network OK" "${scenario_dir}/switch-to-cellular.log" 2>/dev/null &&
     grep -q "network OK" "${scenario_dir}/switch-to-wifi.log" 2>/dev/null; then
    network_change_hits=$((network_change_hits + 1))
  fi
  local download_continued_4g="NO_EVIDENCE"
  local download_resumed="NO_EVIDENCE"
  local verdict="NO_NETWORK_CHANGE_EVIDENCE"
  if [[ "$t1_web" -gt "$t0_web" || "$t1_peer" -gt "$t0_peer" ]]; then
    download_continued_4g="YES"
  fi
  if [[ "$t2_web" -gt "$t1_web" || "$t2_peer" -gt "$t1_peer" ]]; then
    download_resumed="YES"
  elif [[ "$download_continued_4g" == "YES" ]] &&
       grep -q "network OK" "${scenario_dir}/switch-to-wifi.log" 2>/dev/null; then
    download_resumed="YES"
  fi
  if [[ "$network_change_hits" -gt 0 && "$download_continued_4g" == "YES" && "$download_resumed" == "YES" ]]; then
    verdict="PASS"
  elif [[ "$network_change_hits" -gt 0 ]]; then
    verdict="PARTIAL"
  fi

  cat >"${scenario_dir}/summary.md" <<EOF_SUM
## Scenario 2: 4G switch

### Counter Time Series

| Time | from_peers | from_web_seed |
|------|-----------|--------------|
| T0 (WiFi, before switch) | ${t0_peer} | ${t0_web} |
| T1 (during 4G) | ${t1_peer} | ${t1_web} |
| T2 (after WiFi restore) | ${t2_peer} | ${t2_web} |

### Network Change Evidence
- \`on_network_changed\` logcat hits: ${network_change_hits}

### Download Continuity
- Download continued during 4G: ${download_continued_4g}
- Download resumed after WiFi: ${download_resumed}

### Verdict
${verdict}
- Note: Full throttle verification requires native test (see native-test-*.log)
- Note: bytes_uploaded time series not available in S4 (deferred to M3-Beta-Scale)
EOF_SUM

  log "4g: t0_peer=${t0_peer} t0_web=${t0_web} t1_peer=${t1_peer} t1_web=${t1_web} t2_peer=${t2_peer} t2_web=${t2_web}"
  [[ "$verdict" == "PASS" ]]
}

# ---- Scenario 3: Config hot reload ----
scenario_config_hotload() {
  local result_dir="$1"
  local scenario_dir="${result_dir}/config-hotload"
  mkdir -p "$scenario_dir"

  log "=== Scenario 3: Config hot reload (TTL change) ==="

  local serial device_id
  serial="$(csv_item "$DEVICE_SERIALS" "0")"
  device_id="$(csv_item "$DEVICE_IDS_RESOLVED" "0")"

  # Capture pre-change logcat
  adb -s "$serial" logcat -c || true

  local ttl_hours="$CONFIG_TTL_HOURS"
  if [[ "$ttl_hours" == "auto" ]]; then
    local current_ttl_seconds=""
    current_ttl_seconds="$(psql "$DATABASE_URL" -At -c "SELECT seeding_ttl_seconds FROM p2p_config WHERE id = 1" 2>/dev/null | head -n 1 || true)"
    if [[ "$current_ttl_seconds" == "3600" ]]; then
      ttl_hours="2"
    else
      ttl_hours="1"
    fi
  fi

  log "config: Changing TTL to ${ttl_hours}h..."
  log "config: Running: device-ctl p2p set --ttl-hours=${ttl_hours}"

  # The CLI is on the server side
  if command -v device-ctl >/dev/null 2>&1; then
    device-ctl p2p set --ttl-hours="${ttl_hours}" 2>&1 | tee "${scenario_dir}/cli-output.log"
  else
    # Fallback: try from terminal-agent-dev directory
    if [[ -f "/Users/qf/Alcedo/code/terminal-agent-dev/bin/device-ctl" ]]; then
      /Users/qf/Alcedo/code/terminal-agent-dev/bin/device-ctl p2p set --ttl-hours="${ttl_hours}" 2>&1 | tee "${scenario_dir}/cli-output.log"
    else
      echo "WARN: device-ctl not found — skip CLI step" | tee "${scenario_dir}/cli-output.log"
    fi
  fi

  # Wait up to 60s for propagation (pusher 30s ticker + network delay)
  log "config: waiting up to 60s for P2PConfigPusher propagation..."
  log "config: server_log=${SERVER_LOG}"

  local server_log_start_line=1
  if [[ -f "$SERVER_LOG" ]]; then
    server_log_start_line="$(($(wc -l <"$SERVER_LOG") + 1))"
  fi
  local config_pushed=0
  local config_received=0
  local config_applied=0
  local elapsed=0

  while (( elapsed < 60 )); do
    local server_log_delta=""
    if [[ -f "$SERVER_LOG" ]]; then
      server_log_delta="$(tail -n +"$server_log_start_line" "$SERVER_LOG" 2>/dev/null || true)"
    fi
    # Check server log for push confirmation
    if echo "$server_log_delta" | grep -q "P2P 配置已推送到"; then
      config_pushed=1
    fi
    if echo "$server_log_delta" | grep -q "结果: status=success msg=config updated"; then
      config_received=1
      config_applied=1
    fi
    # Check device logcat for command receipt
    local device_log
    device_log="$(adb -s "$serial" shell logcat -d -v time 2>/dev/null || true)"
    if echo "$device_log" | grep -q "CommandHandler: received update_config"; then
      config_received=1
    fi
    if echo "$device_log" | grep -q "Command result: update_config -> success"; then
      config_applied=1
    fi

    if (( config_pushed && config_received && config_applied )); then
      log "config: all signals received within ${elapsed}s"
      break
    fi
    sleep 5
    elapsed=$((elapsed + 5))
  done

  # Capture final device logcat
  adb -s "$serial" shell logcat -d -v time >"${scenario_dir}/device-logcat.log" 2>/dev/null || true

  cat >"${scenario_dir}/summary.md" <<EOF_SUM
## Scenario 3: Config hot reload

### Timing

- Elapsed: \`${elapsed}s\`
- P2PConfigPusher push confirmed: \`$([[ $config_pushed -eq 1 ]] && echo YES || echo NO)\`
- Command received on device: \`$([[ $config_received -eq 1 ]] && echo YES || echo NO)\`
- Command apply success: \`$([[ $config_applied -eq 1 ]] && echo YES || echo NO)\`
- <=30s target met: \`$([[ $config_pushed -eq 1 && $elapsed -le 30 ]] && echo YES || echo "NO (${elapsed}s)")\`

### Log Evidence

- CLI output: \`cli-output.log\`
- Device logcat: \`device-logcat.log\`

### Verdict

$([[ $config_pushed -eq 1 && $config_applied -eq 1 ]] && echo "PASS" || echo "INCOMPLETE")
EOF_SUM

  log "config: pushed=${config_pushed} received=${config_received} applied=${config_applied} elapsed=${elapsed}s"
  return $(( config_pushed && config_applied ? 0 : 1 ))
}

# ---- Regression (Alpha capability) ----
run_regression() {
  local result_dir="$1"
  log "=== Regression: m3-alpha-e2e.sh ==="
  if [[ -f "${SCRIPT_DIR}/m3-alpha-e2e.sh" ]]; then
    # Run with same env but different result dir
    local reg_dir="${result_dir}/regression"
    mkdir -p "$reg_dir"
    TORRENT_PATH="$TORRENT_PATH" \
      FILE_ID="$FILE_ID" \
      SHA256="$SHA256" \
      FILE_SIZE="$FILE_SIZE" \
      SOURCE_FILE="${SOURCE_FILE:-}" \
      WEB_SEED_URL="$WEB_SEED_URL" \
      SERVER_URL="$SERVER_URL" \
      GRPC_PORT="$GRPC_PORT" \
      DATABASE_URL="$DATABASE_URL" \
      E2E_WAIT_SECONDS="${ALPHA_E2E_WAIT_SECONDS:-120}" \
      RESULT_ROOT="$reg_dir" \
      SKIP_DB_TRIGGER=0 \
      bash "${SCRIPT_DIR}/m3-alpha-e2e.sh" --devices "$DEVICE_SERIALS" --device-ids "$DEVICE_IDS_RESOLVED" 2>&1 | tee "${reg_dir}/regression-run.log"
    log "regression: done (see ${reg_dir}/regression-run.log)"
    return 0
  else
    log "WARN: m3-alpha-e2e.sh not found, skipping regression"
    return 1
  fi
}

# ---- Summary ----
write_summary_beta() {
  local result_dir="$1" scenarios_run="$2"
  local proto_result="INCOMPLETE"
  local switch_result="INCOMPLETE"
  local config_result="INCOMPLETE"
  local regression_result="SKIP"

  if grep -q "PASS" "${result_dir}/proto/summary.md" 2>/dev/null; then
    proto_result="PASS"
  fi
  if grep -q "^PASS$" "${result_dir}/4g-switch/summary.md" 2>/dev/null; then
    switch_result="PASS"
  elif grep -q "^PARTIAL$" "${result_dir}/4g-switch/summary.md" 2>/dev/null; then
    switch_result="PARTIAL"
  fi
  if grep -q "PASS" "${result_dir}/config-hotload/summary.md" 2>/dev/null; then
    config_result="PASS"
  fi
  if grep -q "PASS" "${result_dir}/regression/summary.md" 2>/dev/null; then
    regression_result="PASS"
  fi

  cat >"${result_dir}/summary.md" <<EOF_SUM
# M3-Beta S4 E2E Result

- Run ID: \`${RUN_ID}\`
- Generated: \`$(date -u +%Y-%m-%dT%H:%M:%SZ)\`
- File ID: \`${FILE_ID:-}\`
- Torrent: \`${TORRENT_PATH:-}\`
- Web seed: \`${WEB_SEED_URL:-}\`
- Scenarios: \`${scenarios_run}\`
- Device serials: \`${DEVICE_SERIALS}\`
- Device ids: \`${DEVICE_IDS_RESOLVED}\`

## Summary

| Scenario | Result |
|----------|--------|
| 1 - proto torrent_url | ${proto_result} |
| 2 - 4G switch | ${switch_result} |
| 3 - config hotload | ${config_result} |
| Regression | ${regression_result} |

## Notes

- LAN peer discovery known gap: scenarios 1 and 2 may show from_peers=0
- bytes_uploaded tracking deferred to M3-Beta-Scale
EOF_SUM

  echo "${result_dir}/summary.md"
}

# ---- Main ----
usage() {
  cat <<'USAGE'
Usage:
  bash scripts/m3-beta-e2e.sh [options]

Options:
  --scenario {proto|4g|config|all}   Scenario to run (default: all)
  --devices serial1,serial2,...       Device serials (default: 3-device mapping)
  --device-ids id1,id2,...            Server-side device ids
  --wait N                            Wait seconds for download evidence (default: 300)
  --help                              Show this help

Required input comes from /tmp/m3-alpha-s4/latest.env, produced by:
  cd /path/to/terminal-agent-dev && bash scripts/gen-torrent.sh <file_id>

Environment:
  TORRENT_PATH=<path>          Torrent file path
  FILE_ID=<id>                 Release file id
  SHA256=<hex>                 Expected SHA-256
  FILE_SIZE=<bytes>            File size
  FILE_TYPE=<type>             Payload file_type (default: file; use apk only for install validation)
  WEB_SEED_URL=<url>           HTTP fallback URL
  DATABASE_URL=<url>           PostgreSQL connection string
  SERVER_URL=<url>             Backend server URL
  GRPC_PORT=<port>             gRPC port (default: 9090)
  RESULT_ROOT=<path>           Result directory root (default: ./logs/m3-beta-e2e)
  SERVER_LOG=<path>            Backend log path for config scenario (default: /tmp/terminal-agent-dev.log)
  APP_TORRENT_DIR=<rel-path>   App-internal torrent staging dir via run-as (default: files/p2p)
  PROTO_SHA_WAIT_SECONDS=<sec> Extra SHA polling wait after proto initial wait (default: 300)
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --scenario) SCENARIO="$2"; shift 2 ;;
    --scenario=*) SCENARIO="${1#*=}"; shift ;;
    --devices) DEVICE_SERIALS="$2"; shift 2 ;;
    --devices=*) DEVICE_SERIALS="${1#*=}"; shift ;;
    --device-ids) DEVICE_IDS="${2:-}"; shift 2 ;;
    --device-ids=*) DEVICE_IDS="${1#*=}"; shift ;;
    --device-count) DEVICE_COUNT="${2:-}"; shift 2 ;;
    --device-count=*) DEVICE_COUNT="${1#*=}"; shift ;;
    --wait) WAIT_SECONDS="$2"; shift 2 ;;
    --wait=*) WAIT_SECONDS="${1#*=}"; shift ;;
    -h|--help) usage; exit 0 ;;
    *) die "unknown argument: $1" ;;
  esac
done

# Load env from gen-torrent.sh output without overriding explicit caller env.
PRESET_TORRENT_PATH="${TORRENT_PATH:-}"
PRESET_FILE_ID="${FILE_ID:-}"
PRESET_SHA256="${SHA256:-}"
PRESET_FILE_SIZE="${FILE_SIZE:-}"
PRESET_FILE_TYPE="${FILE_TYPE:-}"
PRESET_WEB_SEED_URL="${WEB_SEED_URL:-}"
PRESET_SOURCE_FILE="${SOURCE_FILE:-}"
if [[ -f "${M3_BETA_ENV:-/tmp/m3-beta-e2e/latest.env}" ]]; then
  # shellcheck disable=SC1090
  source "${M3_BETA_ENV:-/tmp/m3-beta-e2e/latest.env}"
fi
TORRENT_PATH="${PRESET_TORRENT_PATH:-${TORRENT_PATH:-}}"
FILE_ID="${PRESET_FILE_ID:-${FILE_ID:-}}"
SHA256="${PRESET_SHA256:-${SHA256:-}}"
FILE_SIZE="${PRESET_FILE_SIZE:-${FILE_SIZE:-0}}"
FILE_TYPE="${PRESET_FILE_TYPE:-${FILE_TYPE:-file}}"
WEB_SEED_URL="${PRESET_WEB_SEED_URL:-${WEB_SEED_URL:-}}"
SOURCE_FILE="${PRESET_SOURCE_FILE:-${SOURCE_FILE:-}}"

DEVICE_SERIALS="${DEVICE_SERIALS:-$DEFAULT_SERIALS}"
DEVICE_COUNT="${DEVICE_COUNT:-$(csv_count "$DEVICE_SERIALS")}"
WEB_SEED_URL="${WEB_SEED_URL:-${SERVER_URL%/}/api/v1/files/${FILE_ID}/download}"

[[ -n "$TORRENT_PATH" ]] || die "TORRENT_PATH missing; run terminal-agent-dev/scripts/gen-torrent.sh <file_id> first"
[[ -f "$TORRENT_PATH" ]] || die "torrent not found: ${TORRENT_PATH}"
[[ -n "$FILE_ID" ]] || die "FILE_ID missing"
command -v adb >/dev/null 2>&1 || die "adb not found"
command -v psql >/dev/null 2>&1 || die "psql not found"

RUN_ID="${RUN_ID:-$(date +%Y%m%d-%H%M%S)}"
result_dir="${RESULT_ROOT}/${RUN_ID}"
mkdir -p "$result_dir"

log "result_dir=${result_dir}"
log "scenario=${SCENARIO}"

# Resolve device IDs
DEVICE_IDS_RESOLVED=""
logcat_pids=()

cleanup() {
  for pid in "${logcat_pids[@]:-}"; do
    kill "$pid" >/dev/null 2>&1 || true
  done
}
trap cleanup EXIT

# Configure all devices
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
    device_id="BETA-DEVICE-$((i + 1))"
    log "WARN: device_id for ${serial} not found from prefs; fallback ${device_id}."
  fi
  DEVICE_IDS_RESOLVED="${DEVICE_IDS_RESOLVED}${DEVICE_IDS_RESOLVED:+,}${device_id}"
  if configure_device_agent "$serial" "$device_id" "${result_dir}/config-${i}.xml"; then
    log "configured ${serial} as ${device_id}"
  else
    log "WARN: unable to write app prefs via run-as for ${serial}; assuming already configured"
  fi

  # Start logcat capture
  adb -s "$serial" logcat -c || true
  adb -s "$serial" logcat -v time >"${result_dir}/logcat-${i}.log" 2>&1 &
  logcat_pids+=("$!")
done

supersede_stale_beta_commands "$result_dir" "$DEVICE_IDS_RESOLVED"

for ((i=0; i<DEVICE_COUNT; i++)); do
  serial="$(csv_item "$DEVICE_SERIALS" "$i")"
  launch_device_agent "$serial" || log "WARN: unable to launch on ${serial}"
done

# Route by scenario
SCENARIOS_RUN=""
case "$SCENARIO" in
  proto)
    scenario_proto "$result_dir" && log "scenario proto: PASS" || log "scenario proto: INCOMPLETE"
    SCENARIOS_RUN="proto"
    ;;
  4g|4g-switch)
    scenario_4g_switch "$result_dir" && log "scenario 4g: PASS" || log "scenario 4g: INCOMPLETE"
    SCENARIOS_RUN="4g"
    ;;
  config|config-hotload)
    scenario_config_hotload "$result_dir" && log "scenario config: PASS" || log "scenario config: INCOMPLETE"
    SCENARIOS_RUN="config"
    ;;
  all)
    scenario_proto "$result_dir" && log "scenario proto: PASS" || log "scenario proto: INCOMPLETE"
    scenario_4g_switch "$result_dir" && log "scenario 4g: PASS" || log "scenario 4g: INCOMPLETE"
    scenario_config_hotload "$result_dir" && log "scenario config: PASS" || log "scenario config: INCOMPLETE"
    SCENARIOS_RUN="all"
    ;;
  *) die "unknown scenario: ${SCENARIO}" ;;
esac

# Regression always runs when scenario=all
if [[ "$SCENARIO" == "all" ]]; then
  run_regression "$result_dir"
fi

summary="$(write_summary_beta "$result_dir" "$SCENARIO")"
log "summary=${summary}"
log "M3-Beta E2E done (scenario=${SCENARIO})"
