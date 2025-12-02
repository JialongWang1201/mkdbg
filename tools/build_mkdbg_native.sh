#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
OUT="${BUILD_DIR}/mkdbg-native"
OUT_ARTIFACT=""

detect_host_os() {
  case "$(uname -s)" in
    Linux) printf '%s\n' "linux" ;;
    Darwin) printf '%s\n' "darwin" ;;
    *)
      echo "error: unsupported host OS: $(uname -s)" >&2
      exit 2
      ;;
  esac
}

detect_host_arch() {
  case "$(uname -m)" in
    x86_64|amd64) printf '%s\n' "x86_64" ;;
    arm64|aarch64) printf '%s\n' "arm64" ;;
    *)
      echo "error: unsupported host arch: $(uname -m)" >&2
      exit 2
      ;;
  esac
}

HOST_OS="${MKDBG_NATIVE_HOST_OS:-$(detect_host_os)}"
HOST_ARCH="${MKDBG_NATIVE_HOST_ARCH:-$(detect_host_arch)}"
ARTIFACT_BASENAME="${MKDBG_NATIVE_ARTIFACT_BASENAME:-mkdbg-native-${HOST_OS}-${HOST_ARCH}}"
OUT="${MKDBG_NATIVE_OUTPUT:-${OUT}}"
OUT_ARTIFACT="${MKDBG_NATIVE_ARTIFACT_OUTPUT:-${BUILD_DIR}/${ARTIFACT_BASENAME}}"

mkdir -p "${BUILD_DIR}"

cc -std=c99 -Wall -Wextra -Werror -O2 \
  -o "${OUT}" \
  "${ROOT_DIR}/tools/mkdbg_native.c" \
  "${ROOT_DIR}/tools/mkdbg_util.c" \
  "${ROOT_DIR}/tools/mkdbg_process.c" \
  "${ROOT_DIR}/tools/mkdbg_config.c" \
  "${ROOT_DIR}/tools/mkdbg_incident.c" \
  "${ROOT_DIR}/tools/mkdbg_parse.c"

if [[ "${OUT_ARTIFACT}" != "${OUT}" ]]; then
  cp "${OUT}" "${OUT_ARTIFACT}"
fi

echo "built ${OUT}"
echo "built ${OUT_ARTIFACT}"
