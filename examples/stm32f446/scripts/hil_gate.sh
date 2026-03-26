#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
LOG_DIR="${ROOT_DIR}/logs"
TIMESTAMP="$(date +%Y%m%d_%H%M%S)"
STEP_FILE="$(mktemp)"
EMIT_JSON=0
KEEP_GOING=0

PORT=""
BAUD="115200"
CHAR_DELAY_MS="10"
RETRIES="2"
SNAPSHOT_TIMEOUT_S="60"
SKIP_BUILD=0
SKIP_FLASH=0
WITH_BOARD=0
WITH_PROFILE=0
REPORT_PATH=""

cleanup() {
  rm -f "${STEP_FILE}"
}
trap cleanup EXIT

usage() {
  cat <<'EOF'
Usage: bash tools/hil_gate.sh --port <serial_port> [options]

Required:
  --port <path>                  Serial device path, e.g. /dev/cu.usbmodemXXXX

Options:
  --baud <int>                   UART baud rate (default: 115200)
  --char-delay-ms <float>        Per-char TX delay in ms (default: 10)
  --retries <int>                Retry count for regress steps (default: 2)
  --snapshot-timeout-s <float>   Snapshot timeout for bringup regress (default: 60)
  --skip-build                   Skip firmware build step
  --skip-flash                   Skip firmware flash step
  --with-board                   Include board_regress step
  --with-profile                 Include profile_regress step
  --keep-going                   Continue executing remaining steps after a failure
  --report-path <path>           Output JSON report path (default: logs/hil_gate_<ts>.json)
  --json                         Print JSON report to stdout
  -h, --help                     Show this help

Default pipeline:
  build -> flash -> bringup_regress -> kdi_driver_regress -> kdi_irq_regress
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --port)
      PORT="${2:-}"
      shift 2
      ;;
    --baud)
      BAUD="${2:-}"
      shift 2
      ;;
    --char-delay-ms)
      CHAR_DELAY_MS="${2:-}"
      shift 2
      ;;
    --retries)
      RETRIES="${2:-}"
      shift 2
      ;;
    --snapshot-timeout-s)
      SNAPSHOT_TIMEOUT_S="${2:-}"
      shift 2
      ;;
    --skip-build)
      SKIP_BUILD=1
      shift
      ;;
    --skip-flash)
      SKIP_FLASH=1
      shift
      ;;
    --with-board)
      WITH_BOARD=1
      shift
      ;;
    --with-profile)
      WITH_PROFILE=1
      shift
      ;;
    --keep-going)
      KEEP_GOING=1
      shift
      ;;
    --report-path)
      REPORT_PATH="${2:-}"
      shift 2
      ;;
    --json)
      EMIT_JSON=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "error: unknown argument: $1" >&2
      usage
      exit 2
      ;;
  esac
done

if [[ -z "${PORT}" ]]; then
  echo "error: --port is required" >&2
  usage
  exit 2
fi

mkdir -p "${LOG_DIR}"

if [[ -z "${REPORT_PATH}" ]]; then
  REPORT_PATH="${LOG_DIR}/hil_gate_${TIMESTAMP}.json"
fi

if [[ "${REPORT_PATH}" != /* ]]; then
  REPORT_PATH="${ROOT_DIR}/${REPORT_PATH}"
fi

REPORT_DIR="$(dirname "${REPORT_PATH}")"
mkdir -p "${REPORT_DIR}"

json_append_step() {
  python3 - "$STEP_FILE" <<'PY'
import json
import os
import sys

path = sys.argv[1]
record = {
    "name": os.environ["STEP_NAME"],
    "command": os.environ["STEP_COMMAND"],
    "rc": int(os.environ["STEP_RC"]),
    "ok": os.environ["STEP_OK"] == "1",
    "stdout_file": os.environ.get("STEP_STDOUT_FILE", ""),
    "stderr_file": os.environ.get("STEP_STDERR_FILE", ""),
    "log": os.environ.get("STEP_LOG", ""),
    "notes": os.environ.get("STEP_NOTES", ""),
}
with open(path, "a", encoding="utf-8") as f:
    f.write(json.dumps(record, ensure_ascii=True) + "\n")
PY
}

cmd_join() {
  local out=""
  local arg
  for arg in "$@"; do
    out+=$(printf '%q ' "${arg}")
  done
  printf '%s' "${out% }"
}

emit_step_line() {
  local name="$1"
  local status="$2"
  local rc="$3"
  local log_path="$4"
  local notes="$5"
  echo "HIL_STEP name=${name} status=${status} rc=${rc} log=${log_path:-none} notes=${notes:-none}"
}

run_cmd_step() {
  local step_name="$1"
  shift
  local command_string
  local stdout_file="${LOG_DIR}/hil_gate_${TIMESTAMP}_${step_name}.stdout.log"
  local stderr_file="${LOG_DIR}/hil_gate_${TIMESTAMP}_${step_name}.stderr.log"
  local rc=0
  local ok=1
  local status="PASS"
  local notes=""

  command_string="$(cmd_join "$@")"

  set +e
  "$@" >"${stdout_file}" 2>"${stderr_file}"
  rc=$?
  set -e

  if [[ ${rc} -ne 0 ]]; then
    ok=0
    status="FAIL"
    notes="$(head -n 1 "${stderr_file}" || true)"
  fi

  STEP_NAME="${step_name}" \
  STEP_COMMAND="${command_string}" \
  STEP_RC="${rc}" \
  STEP_OK="${ok}" \
  STEP_STDOUT_FILE="${stdout_file}" \
  STEP_STDERR_FILE="${stderr_file}" \
  STEP_LOG="" \
  STEP_NOTES="${notes}" \
    json_append_step

  emit_step_line "${step_name}" "${status}" "${rc}" "" "${notes}"

  if [[ ${ok} -ne 1 && ${KEEP_GOING} -ne 1 ]]; then
    return 1
  fi
  return 0
}

run_json_step() {
  local step_name="$1"
  shift
  local command_string
  local stdout_file="${LOG_DIR}/hil_gate_${TIMESTAMP}_${step_name}.stdout.log"
  local stderr_file="${LOG_DIR}/hil_gate_${TIMESTAMP}_${step_name}.stderr.log"
  local rc=0
  local ok=0
  local status="FAIL"
  local notes=""
  local step_log=""
  local parse_line=""
  local regress_ok=""
  local regress_log=""
  local steps_executed=""
  local steps_total=""

  command_string="$(cmd_join "$@")"

  set +e
  "$@" >"${stdout_file}" 2>"${stderr_file}"
  rc=$?
  set -e

  if parse_line="$(python3 - "${stdout_file}" <<'PY'
import json
import sys

path = sys.argv[1]
text = open(path, "r", encoding="utf-8").read().strip()
if not text:
    raise SystemExit(2)
try:
    obj = json.loads(text)
except Exception:
    raise SystemExit(3)
ok = 1 if obj.get("ok") else 0
log = obj.get("log", "")
steps_executed = obj.get("steps_executed", "")
steps_total = obj.get("steps_total", "")
print(f"{ok}\t{log}\t{steps_executed}\t{steps_total}")
PY
)"; then
    IFS=$'\t' read -r regress_ok regress_log steps_executed steps_total <<< "${parse_line}"
    if [[ "${regress_ok}" == "1" && ${rc} -eq 0 ]]; then
      ok=1
      status="PASS"
    else
      ok=0
      status="FAIL"
      notes="regress_ok=${regress_ok} steps=${steps_executed}/${steps_total}"
    fi
    step_log="${regress_log}"
  else
    ok=0
    status="FAIL"
    notes="json_parse_failed"
    if [[ ${rc} -ne 0 ]]; then
      notes+=" rc=${rc}"
    fi
  fi

  if [[ -z "${notes}" && ${rc} -ne 0 ]]; then
    notes="rc=${rc}"
  fi

  STEP_NAME="${step_name}" \
  STEP_COMMAND="${command_string}" \
  STEP_RC="${rc}" \
  STEP_OK="${ok}" \
  STEP_STDOUT_FILE="${stdout_file}" \
  STEP_STDERR_FILE="${stderr_file}" \
  STEP_LOG="${step_log}" \
  STEP_NOTES="${notes}" \
    json_append_step

  emit_step_line "${step_name}" "${status}" "${rc}" "${step_log}" "${notes}"

  if [[ ${ok} -ne 1 && ${KEEP_GOING} -ne 1 ]]; then
    return 1
  fi
  return 0
}

START_EPOCH="$(date +%s)"

if [[ ${SKIP_BUILD} -ne 1 ]]; then
  run_cmd_step "build" bash "${ROOT_DIR}/tools/build.sh"
fi

if [[ ${SKIP_FLASH} -ne 1 ]]; then
  run_cmd_step "flash" bash "${ROOT_DIR}/tools/flash.sh"
fi

run_json_step "bringup_regress" \
  python3 "${ROOT_DIR}/tools/bringup_regress.py" \
  --port "${PORT}" \
  --baud "${BAUD}" \
  --char-delay-ms "${CHAR_DELAY_MS}" \
  --retries "${RETRIES}" \
  --snapshot-timeout-s "${SNAPSHOT_TIMEOUT_S}" \
  --no-auto-flash-stale \
  --json

run_json_step "kdi_driver_regress" \
  python3 "${ROOT_DIR}/tools/kdi_driver_regress.py" \
  --port "${PORT}" \
  --baud "${BAUD}" \
  --char-delay-ms "${CHAR_DELAY_MS}" \
  --retries "${RETRIES}" \
  --no-auto-flash-stale \
  --json

run_json_step "kdi_irq_regress" \
  python3 "${ROOT_DIR}/tools/kdi_irq_regress.py" \
  --port "${PORT}" \
  --baud "${BAUD}" \
  --char-delay-ms "${CHAR_DELAY_MS}" \
  --retries "${RETRIES}" \
  --no-auto-flash-stale \
  --json

if [[ ${WITH_BOARD} -eq 1 ]]; then
  run_json_step "board_regress" \
    python3 "${ROOT_DIR}/tools/board_regress.py" \
    --port "${PORT}" \
    --baud "${BAUD}" \
    --char-delay-ms "${CHAR_DELAY_MS}" \
    --retries "${RETRIES}" \
    --json
fi

if [[ ${WITH_PROFILE} -eq 1 ]]; then
  run_json_step "profile_regress" \
    python3 "${ROOT_DIR}/tools/driver_profile_compare_regress.py" \
    --port "${PORT}" \
    --baud "${BAUD}" \
    --char-delay-ms "${CHAR_DELAY_MS}" \
    --retries "${RETRIES}" \
    --json
fi

END_EPOCH="$(date +%s)"

START_EPOCH="${START_EPOCH}" \
END_EPOCH="${END_EPOCH}" \
PORT="${PORT}" \
BAUD="${BAUD}" \
CHAR_DELAY_MS="${CHAR_DELAY_MS}" \
RETRIES="${RETRIES}" \
SNAPSHOT_TIMEOUT_S="${SNAPSHOT_TIMEOUT_S}" \
SKIP_BUILD="${SKIP_BUILD}" \
SKIP_FLASH="${SKIP_FLASH}" \
WITH_BOARD="${WITH_BOARD}" \
WITH_PROFILE="${WITH_PROFILE}" \
python3 - "${STEP_FILE}" "${REPORT_PATH}" <<'PY'
import json
import os
import sys
import time
from pathlib import Path

step_file = Path(sys.argv[1])
report_path = Path(sys.argv[2])

records = []
if step_file.exists():
    for line in step_file.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line:
            continue
        records.append(json.loads(line))

steps_total = len(records)
steps_passed = sum(1 for r in records if r.get("ok"))
overall_ok = steps_total > 0 and steps_passed == steps_total

payload = {
    "ok": overall_ok,
    "time": time.strftime("%Y-%m-%d %H:%M:%S"),
    "duration_s": max(0, int(os.environ["END_EPOCH"]) - int(os.environ["START_EPOCH"])),
    "port": os.environ["PORT"],
    "baud": int(os.environ["BAUD"]),
    "char_delay_ms": float(os.environ["CHAR_DELAY_MS"]),
    "retries": int(os.environ["RETRIES"]),
    "snapshot_timeout_s": float(os.environ["SNAPSHOT_TIMEOUT_S"]),
    "skip_build": os.environ["SKIP_BUILD"] == "1",
    "skip_flash": os.environ["SKIP_FLASH"] == "1",
    "with_board": os.environ["WITH_BOARD"] == "1",
    "with_profile": os.environ["WITH_PROFILE"] == "1",
    "steps_total": steps_total,
    "steps_passed": steps_passed,
    "steps": records,
}

report_path.parent.mkdir(parents=True, exist_ok=True)
report_path.write_text(json.dumps(payload, ensure_ascii=True), encoding="utf-8")
PY

SUMMARY_STATUS="FAIL"
if python3 - "${REPORT_PATH}" <<'PY'
import json
import sys

obj = json.loads(open(sys.argv[1], "r", encoding="utf-8").read())
raise SystemExit(0 if obj.get("ok") else 1)
PY
then
  SUMMARY_STATUS="PASS"
fi

echo "HIL_SUMMARY status=${SUMMARY_STATUS} report=${REPORT_PATH}"

if [[ ${EMIT_JSON} -eq 1 ]]; then
  cat "${REPORT_PATH}"
fi

if [[ "${SUMMARY_STATUS}" == "PASS" ]]; then
  exit 0
fi
exit 1
