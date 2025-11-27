#!/usr/bin/env bash
set -euo pipefail

INSTALL_DIR="${MKDBG_INSTALL_DIR:-$HOME/.local/bin}"
TARGET="${INSTALL_DIR}/mkdbg"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LOCAL_NATIVE_SOURCE="${SCRIPT_DIR}/mkdbg_native.c"
LOCAL_PYTHON_SOURCE="${SCRIPT_DIR}/mkdbg"
NATIVE_BINARY_PATH="${MKDBG_INSTALL_BINARY_PATH:-}"
NATIVE_BINARY_URL="${MKDBG_INSTALL_BINARY_URL:-}"
NATIVE_BINARY_BASE_URL="${MKDBG_INSTALL_BINARY_BASE_URL:-}"
REPO_SLUG="${MKDBG_REPO_SLUG:-JialongWang1201/MicroKernel-MPU}"
REPO_REF="${MKDBG_REF:-main}"
INSTALL_FLAVOR="${MKDBG_INSTALL_FLAVOR:-native}"
CC_BIN="${CC:-cc}"
REMOTE_NATIVE_URL="https://raw.githubusercontent.com/${REPO_SLUG}/${REPO_REF}/tools/mkdbg_native.c"
REMOTE_PYTHON_URL="https://raw.githubusercontent.com/${REPO_SLUG}/${REPO_REF}/tools/mkdbg"
INSTALL_MODE="${INSTALL_FLAVOR}"

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

require_curl() {
  if ! command -v curl >/dev/null 2>&1; then
    echo "error: curl is required when installing without a local checkout" >&2
    exit 2
  fi
}

install_native_binary_path() {
  local binary_path="$1"

  if [[ ! -f "${binary_path}" ]]; then
    echo "error: native binary override not found: ${binary_path}" >&2
    exit 2
  fi

  cp "${binary_path}" "${TARGET}"
}

install_native_binary_url() {
  local binary_url="$1"
  local tmp_target="${TARGET}.tmp"

  require_curl
  curl -fsSL "${binary_url}" -o "${tmp_target}"
  mv "${tmp_target}" "${TARGET}"
}

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

resolve_native_binary_url() {
  local base_url="$1"
  local host_os
  local host_arch

  host_os="$(detect_host_os)"
  host_arch="$(detect_host_arch)"
  printf '%s/%s\n' "${base_url%/}" "mkdbg-native-${host_os}-${host_arch}"
}

mkdir -p "${INSTALL_DIR}"

case "${INSTALL_FLAVOR}" in
  native)
    if [[ -n "${NATIVE_BINARY_PATH}" ]]; then
      install_native_binary_path "${NATIVE_BINARY_PATH}"
      INSTALL_MODE="native-binary"
    elif [[ -n "${NATIVE_BINARY_BASE_URL}" ]]; then
      install_native_binary_url "$(resolve_native_binary_url "${NATIVE_BINARY_BASE_URL}")"
      INSTALL_MODE="native-binary"
    elif [[ -n "${NATIVE_BINARY_URL}" ]]; then
      install_native_binary_url "${NATIVE_BINARY_URL}"
      INSTALL_MODE="native-binary"
    elif [[ -f "${LOCAL_NATIVE_SOURCE}" ]]; then
      compile_native "${LOCAL_NATIVE_SOURCE}"
      INSTALL_MODE="native-source"
    else
      require_curl
      TMP_DIR="$(mktemp -d)"
      trap 'rm -rf "${TMP_DIR}"' EXIT
      curl -fsSL "${REMOTE_NATIVE_URL}" -o "${TMP_DIR}/mkdbg_native.c"
      compile_native "${TMP_DIR}/mkdbg_native.c"
      INSTALL_MODE="native-source"
    fi
    ;;
  python)
    if [[ -f "${LOCAL_PYTHON_SOURCE}" ]]; then
      cp "${LOCAL_PYTHON_SOURCE}" "${TARGET}"
    else
      require_curl
      curl -fsSL "${REMOTE_PYTHON_URL}" -o "${TARGET}"
    fi
    ;;
  *)
    echo "error: unsupported MKDBG_INSTALL_FLAVOR=${INSTALL_FLAVOR}" >&2
    exit 2
    ;;
esac

chmod +x "${TARGET}"

echo "installed mkdbg (${INSTALL_MODE}) -> ${TARGET}"
case ":${PATH}:" in
  *:"${INSTALL_DIR}":*)
    ;;
  *)
    echo "add ${INSTALL_DIR} to PATH to call mkdbg directly"
    ;;
esac
