#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
EXAMPLE_DIR="${ROOT_DIR}/examples/stm32f446"
TMP_DIR="$(mktemp -d)"
BUILD_DIR="${TMP_DIR}/build"
HEADER_PATH="${BUILD_DIR}/generated/build_info.h"

cleanup() {
  rm -rf "${TMP_DIR}"
}
trap cleanup EXIT

cmake --fresh -S "${EXAMPLE_DIR}" -B "${BUILD_DIR}" \
  -DCMAKE_TOOLCHAIN_FILE="${EXAMPLE_DIR}/cmake/arm-none-eabi.cmake" \
  -DBUILD_PROFILE=ci-smoke \
  -DBOARD_UART_PORT=3 >/dev/null

python3 - "${HEADER_PATH}" <<'PY'
import re
import sys
from pathlib import Path

text = Path(sys.argv[1]).read_text(encoding="utf-8")
checks = [
    '#define BUILD_INFO_FIRMWARE_NAME "MicroKernel-MPU"',
    '#define BUILD_INFO_BOARD_NAME "Nucleo-F446RE"',
    '#define BUILD_INFO_PROFILE "ci-smoke"',
]
for item in checks:
    if item not in text:
        raise SystemExit(f"missing expected build info field: {item}")

sha_match = re.search(r'^#define BUILD_INFO_GIT_SHA "([^"]+)"$', text, re.MULTILINE)
state_match = re.search(r'^#define BUILD_INFO_GIT_STATE "([^"]+)"$', text, re.MULTILINE)
id_match = re.search(r"^#define BUILD_INFO_ID (0x[0-9A-F]{8}U)$", text, re.MULTILINE)
if not sha_match or not sha_match.group(1):
    raise SystemExit("missing BUILD_INFO_GIT_SHA")
if not state_match or state_match.group(1) not in {"clean", "dirty"}:
    raise SystemExit("unexpected BUILD_INFO_GIT_STATE")
if not id_match:
    raise SystemExit("missing BUILD_INFO_ID")
if sha_match.group(1) != "nogit" and id_match.group(1) == "0x4D4B4442U":
    raise SystemExit("BUILD_INFO_ID unexpectedly fell back despite git sha")
PY

echo "build_identity_host_tests: OK"
