#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
HEADER_PATH="${BUILD_DIR}/generated/build_info.h"

SCRIPTS="${ROOT_DIR}/examples/stm32f446/scripts"

bash "${SCRIPTS}/build_identity_host_tests.sh"
bash "${SCRIPTS}/build_mkdbg_native_host_tests.sh"
bash "${SCRIPTS}/mkdbg_native_cmake_host_tests.sh"
bash "${SCRIPTS}/mkdbg_host_tests.sh"
bash "${SCRIPTS}/mkdbg_native_host_tests.sh"
bash "${SCRIPTS}/install_mkdbg_host_tests.sh"
bash "${SCRIPTS}/ovwatch_host_tests.sh"

BUILD_PROFILE=ci-smoke BOARD_UART_PORT=3 VM32_MEM_SIZE=1024 \
  bash "${SCRIPTS}/build.sh"

python3 - "${HEADER_PATH}" \
  "${BUILD_DIR}/MicroKernel_MPU.elf" \
  "${BUILD_DIR}/MicroKernel_MPU.hex" \
  "${BUILD_DIR}/MicroKernel_MPU.bin" \
  "${BUILD_DIR}/MicroKernel_MPU.map" <<'PY'
import re
import sys
from pathlib import Path

header = Path(sys.argv[1]).read_text(encoding="utf-8")
checks = [
    '#define BUILD_INFO_FIRMWARE_NAME "MicroKernel-MPU"',
    '#define BUILD_INFO_BOARD_NAME "Nucleo-F446RE"',
    '#define BUILD_INFO_PROFILE "ci-smoke"',
]
for item in checks:
    if item not in header:
        raise SystemExit(f"missing expected build info field: {item}")

sha_match = re.search(r'^#define BUILD_INFO_GIT_SHA "([^"]+)"$', header, re.MULTILINE)
id_match = re.search(r"^#define BUILD_INFO_ID (0x[0-9A-F]{8}U)$", header, re.MULTILINE)
if not sha_match or not sha_match.group(1):
    raise SystemExit("missing BUILD_INFO_GIT_SHA")
if not id_match:
    raise SystemExit("missing BUILD_INFO_ID")
if sha_match.group(1) != "nogit" and id_match.group(1) == "0x4D4B4442U":
    raise SystemExit("BUILD_INFO_ID unexpectedly fell back despite git sha")

for path_text in sys.argv[2:]:
    path = Path(path_text)
    if not path.is_file():
        raise SystemExit(f"missing build artifact: {path}")
PY

echo "ci_smoke: OK"
