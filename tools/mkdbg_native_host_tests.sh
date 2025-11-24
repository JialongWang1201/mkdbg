#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TMP_DIR="$(mktemp -d)"
BIN_DIR="${TMP_DIR}/bin"
BUILD_OUT="${TMP_DIR}/build.out"
VERSION_OUT="${TMP_DIR}/version.out"
DOCTOR_OUT="${TMP_DIR}/doctor.out"
REPO_LIST_OUT="${TMP_DIR}/repo-list.out"
TARGET_LIST_OUT="${TMP_DIR}/target-list.out"
INCIDENT_OPEN_OUT="${TMP_DIR}/incident-open.out"
INCIDENT_STATUS_OUT="${TMP_DIR}/incident-status.out"
INCIDENT_STATUS_JSON_OUT="${TMP_DIR}/incident-status.json"
INCIDENT_CLOSE_OUT="${TMP_DIR}/incident-close.out"
CAPTURE_DRY_OUT="${TMP_DIR}/capture-dry.out"
CAPTURE_JSON_OUT="${TMP_DIR}/capture.json"
CAPTURE_STDOUT="${TMP_DIR}/capture.stdout"
CAPTURE_INCIDENT_STDOUT="${TMP_DIR}/capture-incident.stdout"
WATCH_DRY_OUT="${TMP_DIR}/watch-dry.out"
WATCH_RENDER_OUT="${TMP_DIR}/watch-render.out"
ATTACH_DRY_OUT="${TMP_DIR}/attach-dry.out"
ATTACH_ERR_OUT="${TMP_DIR}/attach.err"
PROBE_HALT_OUT="${TMP_DIR}/probe-halt.out"
PROBE_FLASH_OUT="${TMP_DIR}/probe-flash.out"
PROBE_READ32_OUT="${TMP_DIR}/probe-read32.out"
PROBE_WRITE32_OUT="${TMP_DIR}/probe-write32.out"
RUN_OUT="${TMP_DIR}/run.out"
CONFIG_PATH="${TMP_DIR}/.mkdbg.toml"

cleanup() {
  rm -rf "${TMP_DIR}"
}
trap cleanup EXIT

bash "${ROOT_DIR}/tools/build_mkdbg_native.sh" > "${BUILD_OUT}"
test -x "${ROOT_DIR}/build/mkdbg-native"

mkdir -p "${BIN_DIR}"
cat > "${BIN_DIR}/openocd" <<'EOF'
#!/usr/bin/env bash
exit 0
EOF
cat > "${BIN_DIR}/arm-none-eabi-gdb" <<'EOF'
#!/usr/bin/env bash
exit 0
EOF
chmod +x "${BIN_DIR}/openocd" "${BIN_DIR}/arm-none-eabi-gdb"

pushd "${TMP_DIR}" >/dev/null
mkdir -p alt build tools
: > build/MicroKernel_MPU.elf
: > tools/openocd.cfg

PATH="${BIN_DIR}:${PATH}" "${ROOT_DIR}/build/mkdbg-native" --version > "${VERSION_OUT}"
python3 - "${VERSION_OUT}" <<'PY'
import sys
from pathlib import Path

text = Path(sys.argv[1]).read_text(encoding="utf-8")
if "mkdbg-native 0.1.0" not in text:
    raise SystemExit(f"missing native version output: {text!r}")
PY

PATH="${BIN_DIR}:${PATH}" "${ROOT_DIR}/build/mkdbg-native" init \
  --name microkernel \
  --port /dev/ttyACM0 >/dev/null
test -f "${CONFIG_PATH}"

python3 - "${CONFIG_PATH}" <<'PY'
import sys
from pathlib import Path

text = Path(sys.argv[1]).read_text(encoding="utf-8")
checks = [
    'default_repo = "microkernel"',
    '[repos."microkernel"]',
    'preset = "microkernel-mpu"',
    'port = "/dev/ttyACM0"',
]
for item in checks:
    if item not in text:
        raise SystemExit(f"missing expected config line: {item}")
PY

PATH="${BIN_DIR}:${PATH}" "${ROOT_DIR}/build/mkdbg-native" doctor --target microkernel > "${DOCTOR_OUT}"
python3 - "${DOCTOR_OUT}" <<'PY'
import sys
from pathlib import Path

text = Path(sys.argv[1]).read_text(encoding="utf-8")
checks = [
    "[mkdbg] ok      config: ",
    "[mkdbg] ok      root: ",
    "[mkdbg] ok      repo: microkernel",
    "[mkdbg] ok      port: /dev/ttyACM0",
    "[mkdbg] ok      build_cmd: bash",
    "[mkdbg] ok      flash_cmd: bash",
    "[mkdbg] ok      hil_cmd: bash",
    "[mkdbg] ok      snapshot_cmd: python3",
    "[mkdbg] ok      elf_path: ",
    "[mkdbg] ok      openocd_cfg: ",
    "[mkdbg] ok      openocd: openocd",
    "[mkdbg] ok      gdb: arm-none-eabi-gdb",
]
for item in checks:
    if item not in text:
        raise SystemExit(f"missing expected native doctor output: {item}")
PY

PATH="${BIN_DIR}:${PATH}" "${ROOT_DIR}/build/mkdbg-native" repo add lab \
  --path ./alt \
  --preset generic \
  --port /dev/ttyUSB1 \
  --build-cmd make \
  --snapshot-cmd python3 \
  --default >/dev/null

python3 - "${CONFIG_PATH}" <<'PY'
import sys
from pathlib import Path

text = Path(sys.argv[1]).read_text(encoding="utf-8")
checks = [
    'default_repo = "lab"',
    '[repos."lab"]',
    'path = "/',
    'port = "/dev/ttyUSB1"',
    'build_cmd = "make"',
    'preset = "generic"',
]
for item in checks:
    if item not in text:
        raise SystemExit(f"missing expected repo config line: {item}")
PY

PATH="${BIN_DIR}:${PATH}" "${ROOT_DIR}/build/mkdbg-native" repo add rootfixture \
  --path "${ROOT_DIR}" \
  --preset microkernel-mpu >/dev/null

PATH="${BIN_DIR}:${PATH}" "${ROOT_DIR}/build/mkdbg-native" repo list > "${REPO_LIST_OUT}"
PATH="${BIN_DIR}:${PATH}" "${ROOT_DIR}/build/mkdbg-native" target list > "${TARGET_LIST_OUT}"
python3 - "${REPO_LIST_OUT}" "${TARGET_LIST_OUT}" <<'PY'
import sys
from pathlib import Path

repo_text = Path(sys.argv[1]).read_text(encoding="utf-8")
target_text = Path(sys.argv[2]).read_text(encoding="utf-8")
repo_checks = [
    '  microkernel\tpreset=microkernel-mpu\tpath=',
    '* lab\tpreset=generic\tpath=',
]
for item in repo_checks:
    if item not in repo_text:
        raise SystemExit(f"missing expected repo list output: {item}")
if repo_text != target_text:
    raise SystemExit("target list should match repo list output")
PY

PATH="${BIN_DIR}:${PATH}" "${ROOT_DIR}/build/mkdbg-native" target use microkernel >/dev/null
python3 - "${CONFIG_PATH}" <<'PY'
import sys
from pathlib import Path

text = Path(sys.argv[1]).read_text(encoding="utf-8")
if 'default_repo = "microkernel"' not in text:
    raise SystemExit("target use did not restore default repo")
PY

PATH="${BIN_DIR}:${PATH}" "${ROOT_DIR}/build/mkdbg-native" incident open \
  --target microkernel \
  --name "IRQ Timeout" \
  --port /dev/ttyTEST0 > "${INCIDENT_OPEN_OUT}"
PATH="${BIN_DIR}:${PATH}" "${ROOT_DIR}/build/mkdbg-native" incident status > "${INCIDENT_STATUS_OUT}"
PATH="${BIN_DIR}:${PATH}" "${ROOT_DIR}/build/mkdbg-native" incident status --json > "${INCIDENT_STATUS_JSON_OUT}"

python3 - "${INCIDENT_OPEN_OUT}" "${INCIDENT_STATUS_OUT}" "${INCIDENT_STATUS_JSON_OUT}" <<'PY'
import json
import re
import sys
from pathlib import Path

open_text = Path(sys.argv[1]).read_text(encoding="utf-8")
status_text = Path(sys.argv[2]).read_text(encoding="utf-8")
status_json = json.loads(Path(sys.argv[3]).read_text(encoding="utf-8"))
match = re.search(r"incident: ([^\n]+)", open_text)
if match is None:
    raise SystemExit("missing incident id in open output")
incident_id = match.group(1)
checks = [
    f"incident: {incident_id}",
    "status: open",
    "repo: microkernel",
    "port: /dev/ttyTEST0",
]
for item in checks:
    if item not in status_text:
        raise SystemExit(f"missing expected incident status output: {item}")
if not status_json.get("ok") or not status_json.get("active"):
    raise SystemExit(f"unexpected incident status json: {status_json}")
if status_json.get("id") != incident_id:
    raise SystemExit(f"incident id mismatch: {status_json}")
if status_json.get("name") != "IRQ Timeout":
    raise SystemExit(f"incident name mismatch: {status_json}")
if status_json.get("status") != "open":
    raise SystemExit(f"incident status mismatch: {status_json}")
if status_json.get("repo") != "microkernel":
    raise SystemExit(f"incident repo mismatch: {status_json}")
if status_json.get("port") != "/dev/ttyTEST0":
    raise SystemExit(f"incident port mismatch: {status_json}")
PY

PATH="${BIN_DIR}:${PATH}" "${ROOT_DIR}/build/mkdbg-native" incident close > "${INCIDENT_CLOSE_OUT}"
python3 - "${INCIDENT_CLOSE_OUT}" "${INCIDENT_STATUS_JSON_OUT}" <<'PY'
import json
import re
import sys
from pathlib import Path

close_text = Path(sys.argv[1]).read_text(encoding="utf-8")
status_json = json.loads(Path(sys.argv[2]).read_text(encoding="utf-8"))
match = re.search(r'closed incident: ([^\n]+)', close_text)
if match is None:
    raise SystemExit("missing incident id in close output")
if match.group(1) != status_json["id"]:
    raise SystemExit("incident close id mismatch")
PY

INCIDENT_ID="$(sed -n 's/^closed incident: //p' "${INCIDENT_CLOSE_OUT}")"
INCIDENT_META_PATH="${TMP_DIR}/.mkdbg/incidents/${INCIDENT_ID}/incident.json"
test ! -f "${TMP_DIR}/.mkdbg/incidents/current"
test -f "${INCIDENT_META_PATH}"
python3 - "${INCIDENT_META_PATH}" <<'PY'
import json
import sys
from pathlib import Path

meta = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
if meta.get("status") != "closed":
    raise SystemExit(f"incident metadata not closed: {meta}")
if "closed_at" not in meta:
    raise SystemExit(f"incident metadata missing closed_at: {meta}")
PY

PATH="${BIN_DIR}:${PATH}" "${ROOT_DIR}/build/mkdbg-native" incident status --json > "${INCIDENT_STATUS_JSON_OUT}"
python3 - "${INCIDENT_STATUS_JSON_OUT}" <<'PY'
import json
import sys
from pathlib import Path

payload = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
if payload != {"ok": True, "active": False}:
    raise SystemExit(f"expected inactive incident json, got: {payload}")
PY

PATH="${BIN_DIR}:${PATH}" "${ROOT_DIR}/build/mkdbg-native" capture bundle --target microkernel \
  --dry-run > "${CAPTURE_DRY_OUT}"
python3 - "${CAPTURE_DRY_OUT}" <<'PY'
import sys
from pathlib import Path

text = Path(sys.argv[1]).read_text(encoding="utf-8")
checks = [
    "[mkdbg] cwd=",
    "[mkdbg] cmd=",
    "python3",
    "triage_bundle.py",
    "--port",
    "/dev/ttyACM0",
]
for item in checks:
    if item not in text:
        raise SystemExit(f"missing expected capture dry-run output: {item}")
PY

PATH="${BIN_DIR}:${PATH}" "${ROOT_DIR}/build/mkdbg-native" capture bundle --target rootfixture \
  --source-log tests/fixtures/triage/sample_snapshot.log \
  --output "${CAPTURE_JSON_OUT}" \
  --json > "${CAPTURE_STDOUT}"
python3 - "${CAPTURE_JSON_OUT}" "${CAPTURE_STDOUT}" <<'PY'
import json
import sys
from pathlib import Path

bundle = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
stdout_lines = [line for line in Path(sys.argv[2]).read_text(encoding="utf-8").splitlines() if line.strip()]
json_lines = [line for line in stdout_lines if line.lstrip().startswith("{")]
if not json_lines:
    raise SystemExit("expected capture json summary line")
summary = json.loads(json_lines[-1])
if bundle.get("ok") is not True:
    raise SystemExit("expected capture bundle ok=true")
if summary.get("ok") is not True:
    raise SystemExit("expected capture summary ok=true")
if summary.get("bundle_path") != sys.argv[1]:
    raise SystemExit("bundle path mismatch")
if summary.get("fault_slices", 0) < 1:
    raise SystemExit("expected capture fault_slices >= 1")
if bundle.get("dependency", {}).get("target_driver") != "sensor":
    raise SystemExit("expected capture target driver sensor")
PY

PATH="${BIN_DIR}:${PATH}" "${ROOT_DIR}/build/mkdbg-native" incident open \
  --target rootfixture \
  --name "Capture Incident" > "${INCIDENT_OPEN_OUT}"
PATH="${BIN_DIR}:${PATH}" "${ROOT_DIR}/build/mkdbg-native" capture bundle --target rootfixture \
  --source-log tests/fixtures/triage/sample_snapshot.log \
  --json > "${CAPTURE_INCIDENT_STDOUT}"
python3 - "${CAPTURE_INCIDENT_STDOUT}" <<'PY'
import json
import sys
from pathlib import Path

stdout_lines = [line for line in Path(sys.argv[1]).read_text(encoding="utf-8").splitlines() if line.strip()]
json_lines = [line for line in stdout_lines if line.lstrip().startswith("{")]
if not json_lines:
    raise SystemExit("expected capture incident json summary line")
summary = json.loads(json_lines[-1])
bundle_path = Path(summary["bundle_path"])
if bundle_path.name != "bundle.json":
    raise SystemExit("expected default incident bundle name")
if bundle_path.parent.parent.name != "incidents":
    raise SystemExit("expected bundle under .mkdbg/incidents/<id>/bundle.json")
if not bundle_path.exists():
    raise SystemExit("expected incident bundle file to exist")
PY

PATH="${BIN_DIR}:${PATH}" "${ROOT_DIR}/build/mkdbg-native" incident close > "${INCIDENT_CLOSE_OUT}"

PATH="${BIN_DIR}:${PATH}" "${ROOT_DIR}/build/mkdbg-native" watch --target microkernel \
  --dry-run > "${WATCH_DRY_OUT}"
python3 - "${WATCH_DRY_OUT}" <<'PY'
import sys
from pathlib import Path

text = Path(sys.argv[1]).read_text(encoding="utf-8")
checks = [
    "[mkdbg] cwd=",
    "[mkdbg] cmd=",
    "python3",
    "bringup_ui.py",
    "--port",
    "/dev/ttyACM0",
]
for item in checks:
    if item not in text:
        raise SystemExit(f"missing expected watch dry-run output: {item}")
PY

PATH="${BIN_DIR}:${PATH}" "${ROOT_DIR}/build/mkdbg-native" watch --target rootfixture \
  --bundle-json tests/fixtures/triage/sample_bundle.json \
  --render-once \
  --auto-refresh-s 5 \
  --width 120 \
  --height 38 > "${WATCH_RENDER_OUT}"
python3 - "${WATCH_RENDER_OUT}" <<'PY'
import sys
from pathlib import Path

text = Path(sys.argv[1]).read_text(encoding="utf-8")
checks = [
    "MicroKernel-MPU Bringup Console",
    "Hardware-first terminal dashboard for bringup, dependency, and fault triage",
    "failure=early_resource_fault",
    "hypothesis=dma_backpressure",
    "impact drivers:",
    "sensor",
    "auto=5s",
    "age=",
    "g refresh a auto +/- rate 1/2/3 [ ] q",
]
for item in checks:
    if item not in text:
        raise SystemExit(f"missing expected watch render text: {item}")
PY

PATH="${BIN_DIR}:${PATH}" "${ROOT_DIR}/build/mkdbg-native" attach --target microkernel \
  --break main \
  --command continue \
  --command bt \
  --batch \
  --dry-run > "${ATTACH_DRY_OUT}"
python3 - "${ATTACH_DRY_OUT}" <<'PY'
import sys
from pathlib import Path

text = Path(sys.argv[1]).read_text(encoding="utf-8")
checks = [
    "[mkdbg] cwd=",
    "[mkdbg] openocd=openocd -f ",
    "[mkdbg] gdb=arm-none-eabi-gdb ",
    "MicroKernel_MPU.elf",
    "-batch",
    "-ex 'target extended-remote localhost:3333'",
    "-ex 'break main'",
    "-ex continue",
    "-ex bt",
]
for item in checks:
    if item not in text:
        raise SystemExit(f"missing expected attach dry-run output: {item}")
PY

PATH="${BIN_DIR}:${PATH}" "${ROOT_DIR}/build/mkdbg-native" target add tahoe \
  --path . \
  --attach-cmd "gdb build/tahoe.elf" >/dev/null
if PATH="${BIN_DIR}:${PATH}" "${ROOT_DIR}/build/mkdbg-native" attach tahoe --dry-run --break main > /dev/null 2> "${ATTACH_ERR_OUT}"; then
  echo "mkdbg_native_host_tests: expected attach_cmd override to reject scripted flags" >&2
  exit 1
fi
python3 - "${ATTACH_ERR_OUT}" <<'PY'
import sys
from pathlib import Path

text = Path(sys.argv[1]).read_text(encoding="utf-8")
needle = "attach_cmd cannot be combined with --break, --command, or --batch"
if needle not in text:
    raise SystemExit(f"missing expected attach error text: {needle}")
PY

PATH="${BIN_DIR}:${PATH}" "${ROOT_DIR}/build/mkdbg-native" probe halt --target microkernel --dry-run > "${PROBE_HALT_OUT}"
python3 - "${PROBE_HALT_OUT}" <<'PY'
import sys
from pathlib import Path

text = Path(sys.argv[1]).read_text(encoding="utf-8")
checks = [
    "[mkdbg] cwd=",
    "[mkdbg] cmd=openocd -f ",
    "reset halt; shutdown",
]
for item in checks:
    if item not in text:
        raise SystemExit(f"missing expected probe halt output: {item}")
PY

PATH="${BIN_DIR}:${PATH}" "${ROOT_DIR}/build/mkdbg-native" probe flash --target microkernel --dry-run > "${PROBE_FLASH_OUT}"
python3 - "${PROBE_FLASH_OUT}" <<'PY'
import sys
from pathlib import Path

text = Path(sys.argv[1]).read_text(encoding="utf-8")
checks = [
    "[mkdbg] cmd=openocd -f ",
    "program /",
    "MicroKernel_MPU.elf",
    "verify reset exit",
]
for item in checks:
    if item not in text:
        raise SystemExit(f"missing expected probe flash output: {item}")
PY

PATH="${BIN_DIR}:${PATH}" "${ROOT_DIR}/build/mkdbg-native" probe read32 --target microkernel --dry-run 0xE000ED28 > "${PROBE_READ32_OUT}"
python3 - "${PROBE_READ32_OUT}" <<'PY'
import sys
from pathlib import Path

text = Path(sys.argv[1]).read_text(encoding="utf-8")
checks = [
    "mdw 0xe000ed28",
    "shutdown",
]
for item in checks:
    if item not in text:
        raise SystemExit(f"missing expected probe read32 output: {item}")
PY

PATH="${BIN_DIR}:${PATH}" "${ROOT_DIR}/build/mkdbg-native" probe write32 --target microkernel --dry-run 0xE000ED24 0x700 > "${PROBE_WRITE32_OUT}"
python3 - "${PROBE_WRITE32_OUT}" <<'PY'
import sys
from pathlib import Path

text = Path(sys.argv[1]).read_text(encoding="utf-8")
checks = [
    "mww 0xe000ed24 0x00000700",
    "shutdown",
]
for item in checks:
    if item not in text:
        raise SystemExit(f"missing expected probe write32 output: {item}")
PY

PATH="${BIN_DIR}:${PATH}" "${ROOT_DIR}/build/mkdbg-native" run --target rootfixture --dry-run -- echo smoke > "${RUN_OUT}"
python3 - "${RUN_OUT}" <<'PY'
import sys
from pathlib import Path

text = Path(sys.argv[1]).read_text(encoding="utf-8")
checks = [
    "[mkdbg] cwd=",
    "[mkdbg] cmd=echo smoke",
]
for item in checks:
    if item not in text:
        raise SystemExit(f"missing expected run output: {item}")
PY

popd >/dev/null
echo "mkdbg_native_host_tests: OK"
