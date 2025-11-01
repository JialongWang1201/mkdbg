#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
BIN="${BUILD_DIR}/vm32_host_tests"

mkdir -p "${BUILD_DIR}"

compile_and_run() {
  local mem_size="$1"
  echo "== vm32 host tests (VM32_MEM_SIZE=${mem_size}) =="
  cc -std=c99 -Wall -Wextra \
    -DVM32_MEM_SIZE="${mem_size}" \
    -I"${ROOT_DIR}/tests" \
    -I"${ROOT_DIR}/include" \
    -o "${BIN}" \
    "${ROOT_DIR}/tests/vm32_host_tests.c" \
    "${ROOT_DIR}/src/vm32.c"
  "${BIN}"
}

compile_and_run 256
compile_and_run 1024
compile_and_run 4096
