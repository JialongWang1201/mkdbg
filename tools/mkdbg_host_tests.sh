#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TMP_DIR="$(mktemp -d)"
VERSION_OUT="${TMP_DIR}/version.out"
DOCTOR_OUT="${TMP_DIR}/doctor.out"
LIST_OUT="${TMP_DIR}/list.out"
ATTACH_OUT="${TMP_DIR}/attach.out"
ATTACH_ERR="${TMP_DIR}/attach.err"
RUN_OUT="${TMP_DIR}/run.out"
BIN_DIR="${TMP_DIR}/bin"

cleanup() {
  rm -rf "${TMP_DIR}"
}
trap cleanup EXIT

python3 -m py_compile "${ROOT_DIR}/tools/mkdbg"
python3 "${ROOT_DIR}/tools/mkdbg" --version > "${VERSION_OUT}"
python3 - "${VERSION_OUT}" <<'PY'
import sys
from pathlib import Path

text = Path(sys.argv[1]).read_text(encoding="utf-8")
if "mkdbg 0.1.0" not in text:
    raise SystemExit(f"missing version output: {text!r}")
PY

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
mkdir -p build tools
: > build/MicroKernel_MPU.elf
: > tools/openocd.cfg

PATH="${BIN_DIR}:${PATH}" python3 "${ROOT_DIR}/tools/mkdbg" init \
  --name microkernel \
  --port /dev/ttyACM0 >/dev/null

PATH="${BIN_DIR}:${PATH}" python3 "${ROOT_DIR}/tools/mkdbg" doctor > "${DOCTOR_OUT}"
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
        raise SystemExit(f"missing expected doctor output: {item}")
PY

PATH="${BIN_DIR}:${PATH}" python3 "${ROOT_DIR}/tools/mkdbg" attach \
  --dry-run \
  --break main \
  --command continue \
  --command bt \
  --batch > "${ATTACH_OUT}"
python3 - "${ATTACH_OUT}" <<'PY'
import sys
from pathlib import Path

text = Path(sys.argv[1]).read_text(encoding="utf-8")
checks = [
    "[mkdbg] cwd=",
    "[mkdbg] openocd=openocd -f ",
    "[mkdbg] gdb=arm-none-eabi-gdb ",
    "MicroKernel_MPU.elf",
    "-batch",
    "-ex 'break main'",
    "-ex continue",
    "-ex bt",
]
for item in checks:
    if item not in text:
        raise SystemExit(f"missing expected attach output: {item}")
PY

PATH="${BIN_DIR}:${PATH}" python3 "${ROOT_DIR}/tools/mkdbg" repo add tahoe \
  --path . \
  --attach-cmd "gdb build/tahoe.elf" >/dev/null
if PATH="${BIN_DIR}:${PATH}" python3 "${ROOT_DIR}/tools/mkdbg" attach tahoe --dry-run --break main > /dev/null 2> "${ATTACH_ERR}"; then
  echo "mkdbg_host_tests: expected attach_cmd override to reject scripted flags" >&2
  exit 1
fi

python3 - "${ATTACH_ERR}" <<'PY'
import sys
from pathlib import Path

text = Path(sys.argv[1]).read_text(encoding="utf-8")
needle = "attach_cmd cannot be combined with --break, --command, or --batch"
if needle not in text:
    raise SystemExit(f"missing expected error text: {needle}")
PY

PATH="${BIN_DIR}:${PATH}" python3 "${ROOT_DIR}/tools/mkdbg" repo add demo \
  --path . \
  --preset generic \
  --build-cmd "echo build" \
  --flash-cmd "echo flash" \
  --default >/dev/null

PATH="${BIN_DIR}:${PATH}" python3 "${ROOT_DIR}/tools/mkdbg" repo list > "${LIST_OUT}"
python3 - "${LIST_OUT}" <<'PY'
import sys
from pathlib import Path

text = Path(sys.argv[1]).read_text(encoding="utf-8")
checks = [
    "* demo\tpreset=generic\tpath=",
    "  microkernel\tpreset=microkernel-mpu\tpath=",
]
for item in checks:
    if item not in text:
        raise SystemExit(f"missing expected repo list output: {item}")
PY

PATH="${BIN_DIR}:${PATH}" python3 "${ROOT_DIR}/tools/mkdbg" run --repo demo --dry-run -- echo smoke > "${RUN_OUT}"
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

INSTALL_DIR="${TMP_DIR}/install"
MKDBG_INSTALL_DIR="${INSTALL_DIR}" bash "${ROOT_DIR}/tools/install_mkdbg.sh" > /dev/null
test -x "${INSTALL_DIR}/mkdbg"

popd >/dev/null
echo "mkdbg_host_tests: OK"
