#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
BIN="${BUILD_DIR}/kdi_host_tests"

mkdir -p "${BUILD_DIR}"

cc -std=c11 -Wall -Wextra \
  -I"${ROOT_DIR}/include" \
  -I"${ROOT_DIR}/tools/seam/include" \
  -o "${BIN}" \
  "${ROOT_DIR}/tests/kdi_host_tests.c" \
  "${ROOT_DIR}/src/kdi.c" \
  "${ROOT_DIR}/tests/seam_host_stub.c"

"${BIN}"
