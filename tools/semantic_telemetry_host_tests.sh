#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
BIN="${BUILD_DIR}/semantic_telemetry_host_tests"

mkdir -p "${BUILD_DIR}"

cc -std=c99 -Wall -Wextra \
  -I"${ROOT_DIR}/include" \
  -o "${BIN}" \
  "${ROOT_DIR}/tests/semantic_telemetry_host_tests.c" \
  "${ROOT_DIR}/src/semantic_telemetry.c"

"${BIN}"
