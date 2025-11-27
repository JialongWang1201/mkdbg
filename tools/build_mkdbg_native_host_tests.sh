#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TMP_DIR="$(mktemp -d)"
DEFAULT_OUT="${ROOT_DIR}/build/mkdbg-native"
CUSTOM_OUT="${TMP_DIR}/mkdbg-native-custom"
CUSTOM_ARTIFACT="${TMP_DIR}/mkdbg-native-test-linux-x86_64"

cleanup() {
  rm -rf "${TMP_DIR}"
}
trap cleanup EXIT

bash "${ROOT_DIR}/tools/build_mkdbg_native.sh" > /dev/null
test -x "${DEFAULT_OUT}"
"${DEFAULT_OUT}" --version > /dev/null

MKDBG_NATIVE_HOST_OS=linux \
MKDBG_NATIVE_HOST_ARCH=x86_64 \
MKDBG_NATIVE_OUTPUT="${CUSTOM_OUT}" \
MKDBG_NATIVE_ARTIFACT_OUTPUT="${CUSTOM_ARTIFACT}" \
  bash "${ROOT_DIR}/tools/build_mkdbg_native.sh" > /dev/null

test -x "${CUSTOM_OUT}"
test -x "${CUSTOM_ARTIFACT}"
"${CUSTOM_OUT}" --version > /dev/null
"${CUSTOM_ARTIFACT}" --version > /dev/null

echo "build_mkdbg_native_host_tests: OK"
