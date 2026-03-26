#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
EXAMPLE_DIR="${ROOT_DIR}/examples/stm32f446"
BIN="${BUILD_DIR}/vm32_host_tests"

mkdir -p "${BUILD_DIR}"

compile_and_run() {
  local mem_size="$1"
  echo "== vm32 host tests (VM32_MEM_SIZE=${mem_size}) =="
  cc -std=c11 -Wall -Wextra \
    -DVM32_MEM_SIZE="${mem_size}" \
    -I"${ROOT_DIR}/tests" \
    -I"${EXAMPLE_DIR}/include" \
    -I"${ROOT_DIR}/deps/seam/include" \
    -o "${BIN}" \
    "${ROOT_DIR}/tests/vm32_host_tests.c" \
    "${EXAMPLE_DIR}/src/vm32.c" \
    "${ROOT_DIR}/tests/seam_host_stub.c"
  "${BIN}"
}

compile_and_run 256
compile_and_run 1024
compile_and_run 4096
