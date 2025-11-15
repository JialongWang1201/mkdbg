#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TMP_DIR="$(mktemp -d)"
BIN_DIR="${TMP_DIR}/bin"
BUILD_OUT="${TMP_DIR}/build.out"
VERSION_OUT="${TMP_DIR}/version.out"
DOCTOR_OUT="${TMP_DIR}/doctor.out"
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
mkdir -p build tools
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

popd >/dev/null
echo "mkdbg_native_host_tests: OK"
