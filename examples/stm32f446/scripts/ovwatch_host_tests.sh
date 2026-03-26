#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
TMP_DIR="$(mktemp -d)"
DOCTOR_OUT="${TMP_DIR}/doctor.out"
LIST_OUT="${TMP_DIR}/list.out"
PLAN_OUT="${TMP_DIR}/plan.out"
DEBUG_OUT="${TMP_DIR}/debug.out"
MK_PLAN_OUT="${TMP_DIR}/mk.plan.out"

cleanup() {
  rm -rf "${TMP_DIR}"
}
trap cleanup EXIT

python3 -m py_compile "${ROOT_DIR}/tools/ovwatch"

pushd "${TMP_DIR}" >/dev/null

python3 "${ROOT_DIR}/tools/ovwatch" init \
  --adapter generic \
  --name demo \
  --build-cmd "echo build" \
  --debug-cmd "python3 -c 'print(123)'" >/dev/null

python3 "${ROOT_DIR}/tools/ovwatch" doctor > "${DOCTOR_OUT}"
python3 - "${DOCTOR_OUT}" <<'PY'
import sys
from pathlib import Path

text = Path(sys.argv[1]).read_text(encoding="utf-8")
checks = [
    "[ovwatch] ok      root: ",
    "[ovwatch] ok      build_cmd: echo",
    "[ovwatch] ok      debug_cmd: python3",
]
for item in checks:
    if item not in text:
        raise SystemExit(f"missing expected doctor output: {item}")
PY

python3 "${ROOT_DIR}/tools/ovwatch" target add tahoe \
  --adapter tahoeos \
  --path . \
  --build-cmd "make -j4" \
  --run-cmd "echo run" \
  --debug-cmd "./scripts/debug_session.sh" >/dev/null
python3 "${ROOT_DIR}/tools/ovwatch" target list > "${LIST_OUT}"
python3 - "${LIST_OUT}" <<'PY'
import sys
from pathlib import Path

text = Path(sys.argv[1]).read_text(encoding="utf-8")
checks = [
    "* demo\tadapter=generic\tpath=",
    "  tahoe\tadapter=tahoeos\tpath=",
]
for item in checks:
    if item not in text:
        raise SystemExit(f"missing expected target list output: {item}")
PY

python3 "${ROOT_DIR}/tools/ovwatch" attach-plan > "${PLAN_OUT}"
python3 "${ROOT_DIR}/tools/ovwatch" debug --dry-run > "${DEBUG_OUT}"
python3 - "${PLAN_OUT}" "${DEBUG_OUT}" <<'PY'
import sys
from pathlib import Path

plan = Path(sys.argv[1]).read_text(encoding="utf-8")
debug = Path(sys.argv[2]).read_text(encoding="utf-8")
checks = [
    "[ovwatch] target=demo",
    "[ovwatch] mode=custom",
    "[ovwatch] client=python3 -c ",
    "print(123)",
]
for text in (plan, debug):
    for item in checks:
        if item not in text:
            raise SystemExit(f"missing expected debug output: {item}")
PY

mkdir -p mk/build mk/tools
: > mk/build/MicroKernel_MPU.elf
: > mk/tools/openocd.cfg
pushd mk >/dev/null
python3 "${ROOT_DIR}/tools/ovwatch" init \
  --adapter microkernel-mpu \
  --name microkernel \
  --debug-client true >/dev/null
python3 "${ROOT_DIR}/tools/ovwatch" attach-plan > "${MK_PLAN_OUT}"
python3 - "${MK_PLAN_OUT}" <<'PY'
import sys
from pathlib import Path

text = Path(sys.argv[1]).read_text(encoding="utf-8")
checks = [
    "[ovwatch] target=microkernel",
    "[ovwatch] mode=openocd-gdb",
    "[ovwatch] server=openocd -f ",
    "[ovwatch] client=true ",
    "MicroKernel_MPU.elf",
]
for item in checks:
    if item not in text:
        raise SystemExit(f"missing expected microkernel plan output: {item}")
PY
popd >/dev/null

BIN_DIR="$(mktemp -d)"
OVWATCH_INSTALL_DIR="${BIN_DIR}" bash "${ROOT_DIR}/tools/install_ovwatch.sh" > /dev/null
test -x "${BIN_DIR}/ovwatch"
rm -rf "${BIN_DIR}"

popd >/dev/null
echo "ovwatch_host_tests: OK"
