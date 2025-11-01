#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
BIN="${BUILD_DIR}/dependency_graph_host_tests"

mkdir -p "${BUILD_DIR}"

cc -std=c99 -Wall -Wextra \
  -I"${ROOT_DIR}/include" \
  -o "${BIN}" \
  "${ROOT_DIR}/tests/dependency_graph_host_tests.c" \
  "${ROOT_DIR}/src/dependency_graph.c" \
  "${ROOT_DIR}/src/bringup_phase.c"

"${BIN}"
