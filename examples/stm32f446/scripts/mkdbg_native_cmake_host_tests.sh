#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
TMP_DIR="$(mktemp -d)"
BUILD_DIR="${TMP_DIR}/cmake-build"
OUT="${BUILD_DIR}/mkdbg-native"

cleanup() {
  rm -rf "${TMP_DIR}"
}
trap cleanup EXIT

cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release > /dev/null
cmake --build "${BUILD_DIR}" --target mkdbg-native --parallel > /dev/null

test -x "${OUT}"
"${OUT}" --version > /dev/null

echo "mkdbg_native_cmake_host_tests: OK"
