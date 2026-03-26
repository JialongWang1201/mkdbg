#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
EXAMPLE_DIR="${ROOT_DIR}/examples/stm32f446"
BIN="${BUILD_DIR}/semantic_telemetry_host_tests"

mkdir -p "${BUILD_DIR}"

cc -std=c99 -Wall -Wextra \
  -I"${EXAMPLE_DIR}/include" \
  -o "${BIN}" \
  "${ROOT_DIR}/tests/semantic_telemetry_host_tests.c" \
  "${EXAMPLE_DIR}/src/semantic_telemetry.c"

"${BIN}"
