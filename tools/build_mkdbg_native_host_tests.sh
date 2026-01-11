#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TMP_DIR="$(mktemp -d)"
BUILD_DIR="${TMP_DIR}/cmake-build"
DEFAULT_OUT="${BUILD_DIR}/mkdbg-native"
CUSTOM_BUILD="${TMP_DIR}/cmake-custom"
CUSTOM_OUT="${CUSTOM_BUILD}/mkdbg-native"

cleanup() {
  rm -rf "${TMP_DIR}"
}
trap cleanup EXIT

# Default build: cmake to a temp directory (mirrors what install_mkdbg.sh does)
cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release > /dev/null
cmake --build "${BUILD_DIR}" --target mkdbg_native_host --parallel > /dev/null
test -x "${DEFAULT_OUT}"
"${DEFAULT_OUT}" --version > /dev/null

# Second build to a different output directory (verifies cmake works with arbitrary -B paths)
cmake -S "${ROOT_DIR}" -B "${CUSTOM_BUILD}" -DCMAKE_BUILD_TYPE=Release > /dev/null
cmake --build "${CUSTOM_BUILD}" --target mkdbg_native_host --parallel > /dev/null
test -x "${CUSTOM_OUT}"
"${CUSTOM_OUT}" --version > /dev/null

echo "build_mkdbg_native_host_tests: OK"
