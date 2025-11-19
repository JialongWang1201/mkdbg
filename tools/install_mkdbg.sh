#!/usr/bin/env bash
set -euo pipefail

INSTALL_DIR="${MKDBG_INSTALL_DIR:-$HOME/.local/bin}"
TARGET="${INSTALL_DIR}/mkdbg"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LOCAL_NATIVE_SOURCE="${SCRIPT_DIR}/mkdbg_native.c"
LOCAL_PYTHON_SOURCE="${SCRIPT_DIR}/mkdbg"
REPO_SLUG="${MKDBG_REPO_SLUG:-JialongWang1201/MicroKernel-MPU}"
REPO_REF="${MKDBG_REF:-main}"
INSTALL_FLAVOR="${MKDBG_INSTALL_FLAVOR:-native}"
CC_BIN="${CC:-cc}"
REMOTE_NATIVE_URL="https://raw.githubusercontent.com/${REPO_SLUG}/${REPO_REF}/tools/mkdbg_native.c"
REMOTE_PYTHON_URL="https://raw.githubusercontent.com/${REPO_SLUG}/${REPO_REF}/tools/mkdbg"

compile_native() {
  local source_path="$1"
  local tmp_target="${TARGET}.tmp"

  if ! command -v "${CC_BIN}" >/dev/null 2>&1; then
    echo "error: ${CC_BIN} is required to build the native mkdbg installer target" >&2
    exit 2
  fi

  "${CC_BIN}" -std=c99 -Wall -Wextra -Werror -O2 \
    -o "${tmp_target}" \
    "${source_path}"
  mv "${tmp_target}" "${TARGET}"
}

mkdir -p "${INSTALL_DIR}"

case "${INSTALL_FLAVOR}" in
  native)
    if [[ -f "${LOCAL_NATIVE_SOURCE}" ]]; then
      compile_native "${LOCAL_NATIVE_SOURCE}"
    else
      if ! command -v curl >/dev/null 2>&1; then
        echo "error: curl is required when installing without a local checkout" >&2
        exit 2
      fi
      TMP_DIR="$(mktemp -d)"
      trap 'rm -rf "${TMP_DIR}"' EXIT
      curl -fsSL "${REMOTE_NATIVE_URL}" -o "${TMP_DIR}/mkdbg_native.c"
      compile_native "${TMP_DIR}/mkdbg_native.c"
    fi
    ;;
  python)
    if [[ -f "${LOCAL_PYTHON_SOURCE}" ]]; then
      cp "${LOCAL_PYTHON_SOURCE}" "${TARGET}"
    else
      if ! command -v curl >/dev/null 2>&1; then
        echo "error: curl is required when installing without a local checkout" >&2
        exit 2
      fi
      curl -fsSL "${REMOTE_PYTHON_URL}" -o "${TARGET}"
    fi
    ;;
  *)
    echo "error: unsupported MKDBG_INSTALL_FLAVOR=${INSTALL_FLAVOR}" >&2
    exit 2
    ;;
esac

chmod +x "${TARGET}"

echo "installed mkdbg (${INSTALL_FLAVOR}) -> ${TARGET}"
case ":${PATH}:" in
  *:"${INSTALL_DIR}":*)
    ;;
  *)
    echo "add ${INSTALL_DIR} to PATH to call mkdbg directly"
    ;;
esac
