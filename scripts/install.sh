#!/usr/bin/env bash
set -euo pipefail

INSTALL_DIR="${MKDBG_INSTALL_DIR:-$HOME/.local/bin}"
TARGET="${INSTALL_DIR}/mkdbg"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LOCAL_NATIVE_SOURCE="${SCRIPT_DIR}/../src/main.c"
LOCAL_PYTHON_SOURCE="${SCRIPT_DIR}/../examples/stm32f446/scripts/mkdbg"
NATIVE_BINARY_PATH="${MKDBG_INSTALL_BINARY_PATH:-}"
NATIVE_BINARY_URL="${MKDBG_INSTALL_BINARY_URL:-}"
REPO_SLUG="${MKDBG_REPO_SLUG:-JialongWang1201/mkdbg}"
NATIVE_BINARY_BASE_URL="${MKDBG_INSTALL_BINARY_BASE_URL:-https://github.com/${REPO_SLUG}/releases/latest/download}"
REPO_REF="${MKDBG_REF:-main}"
INSTALL_FLAVOR="${MKDBG_INSTALL_FLAVOR:-native}"
CC_BIN="${CC:-cc}"
REMOTE_NATIVE_URL="https://raw.githubusercontent.com/${REPO_SLUG}/${REPO_REF}/tools/mkdbg_native.c"
REMOTE_PYTHON_URL="https://raw.githubusercontent.com/${REPO_SLUG}/${REPO_REF}/tools/mkdbg"
INSTALL_MODE="${INSTALL_FLAVOR}"

compile_native() {
  local source_path="$1"
  local src_dir
  src_dir="$(cd "$(dirname "${source_path}")" && pwd)"

  # Use CMake when in a local checkout (multi-file build)
  if [[ -f "${src_dir}/../CMakeLists.txt" ]] && command -v cmake >/dev/null 2>&1; then
    local repo_root
    local build_dir
    repo_root="$(cd "${src_dir}/.." && pwd)"
    build_dir="$(mktemp -d)"
    cmake --fresh -S "${repo_root}" -B "${build_dir}" -DCMAKE_BUILD_TYPE=Release >/dev/null
    cmake --build "${build_dir}" --target mkdbg-native >/dev/null
    cp "${build_dir}/mkdbg-native" "${TARGET}"
    rm -rf "${build_dir}"
    return
  fi

  # Single-source fallback for remote/standalone installs
  if ! command -v "${CC_BIN}" >/dev/null 2>&1; then
    echo "error: ${CC_BIN} is required to build the native mkdbg installer target" >&2
    exit 2
  fi

  "${CC_BIN}" -std=c99 -Wall -Wextra -Werror -O2 \
    -o "${TARGET}.tmp" \
    "${source_path}"
  mv "${TARGET}.tmp" "${TARGET}"
}

require_curl() {
  if ! command -v curl >/dev/null 2>&1; then
    echo "error: curl is required when installing without a local checkout" >&2
    exit 2
  fi
}

require_git() {
  if ! command -v git >/dev/null 2>&1; then
    echo "error: git is required for remote installation (used to clone source + submodules)" >&2
    exit 2
  fi
}

require_cmake() {
  if ! command -v cmake >/dev/null 2>&1; then
    echo "error: cmake is required to build mkdbg from source" >&2
    exit 2
  fi
}

clone_and_build() {
  local tmp_dir="$1"
  echo "Cloning mkdbg (${REPO_SLUG}@${REPO_REF})..."
  git clone --recurse-submodules --depth 1 --shallow-submodules \
    --branch "${REPO_REF}" \
    "https://github.com/${REPO_SLUG}.git" \
    "${tmp_dir}/mkdbg" >/dev/null 2>&1
  echo "Building mkdbg-native..."
  local build_dir="${tmp_dir}/build"
  cmake -S "${tmp_dir}/mkdbg" -B "${build_dir}" \
    -DCMAKE_BUILD_TYPE=Release >/dev/null 2>&1
  cmake --build "${build_dir}" --target mkdbg-native >/dev/null 2>&1
  cp "${build_dir}/mkdbg-native" "${TARGET}"
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

# Try to download a pre-built binary; returns 0 on success, 1 if not found.
try_prebuilt() {
  local base_url="$1"
  local binary_url
  binary_url="$(resolve_native_binary_url "${base_url}")"
  local tmp_target="${TARGET}.tmp"
  if curl -fsSL "${binary_url}" -o "${tmp_target}" 2>/dev/null; then
    mv "${tmp_target}" "${TARGET}"
    return 0
  fi
  rm -f "${tmp_target}"
  return 1
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
      # Explicit local binary override
      install_native_binary_path "${NATIVE_BINARY_PATH}"
      INSTALL_MODE="native-binary"
    elif [[ -n "${NATIVE_BINARY_URL}" ]]; then
      # Explicit binary URL override
      install_native_binary_url "${NATIVE_BINARY_URL}"
      INSTALL_MODE="native-binary"
    elif [[ -f "${LOCAL_NATIVE_SOURCE}" ]]; then
      # Running from a local checkout — build with CMake
      compile_native "${LOCAL_NATIVE_SOURCE}"
      INSTALL_MODE="native-source"
    else
      # Remote install: try pre-built binary first (fast, zero build-tool deps),
      # fall back to git clone + cmake if no release binary exists yet.
      require_curl
      echo "Trying pre-built binary..."
      if try_prebuilt "${NATIVE_BINARY_BASE_URL}"; then
        INSTALL_MODE="native-binary"
      else
        echo "No pre-built binary found — building from source (requires git + cmake)..."
        require_git
        require_cmake
        TMP_DIR="$(mktemp -d)"
        trap 'rm -rf "${TMP_DIR}"' EXIT
        clone_and_build "${TMP_DIR}"
        INSTALL_MODE="native-source"
      fi
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

# ── wire-host: install alongside mkdbg when built from a local checkout ──────
WIRE_HOST_BUILD="${SCRIPT_DIR}/../build_host/wire-host"
WIRE_HOST_TARGET="${INSTALL_DIR}/wire-host"
if [[ -f "${WIRE_HOST_BUILD}" ]]; then
  cp "${WIRE_HOST_BUILD}" "${WIRE_HOST_TARGET}"
  chmod +x "${WIRE_HOST_TARGET}"
  echo "installed wire-host -> ${WIRE_HOST_TARGET}"
fi

echo "installed mkdbg (${INSTALL_MODE}) -> ${TARGET}"
case ":${PATH}:" in
  *:"${INSTALL_DIR}":*)
    ;;
  *)
    echo "add ${INSTALL_DIR} to PATH to call mkdbg directly"
    ;;
esac
