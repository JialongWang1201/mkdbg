#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TMP_DIR="$(mktemp -d)"
INSTALL_DIR="${TMP_DIR}/install"
INSTALL_OUT="${TMP_DIR}/install.out"
VERSION_OUT="${TMP_DIR}/version.out"
BINARY_INSTALL_DIR="${TMP_DIR}/binary-install"
BINARY_INSTALL_OUT="${TMP_DIR}/binary-install.out"
BINARY_VERSION_OUT="${TMP_DIR}/binary-version.out"
AUTO_INSTALL_DIR="${TMP_DIR}/auto-install"
AUTO_INSTALL_OUT="${TMP_DIR}/auto-install.out"
AUTO_VERSION_OUT="${TMP_DIR}/auto-version.out"
RELEASE_DIR="${TMP_DIR}/release"

detect_host_os() {
  case "$(uname -s)" in
    Linux) printf '%s\n' "linux" ;;
    Darwin) printf '%s\n' "darwin" ;;
    *)
      echo "unsupported host OS: $(uname -s)" >&2
      exit 2
      ;;
  esac
}

detect_host_arch() {
  case "$(uname -m)" in
    x86_64|amd64) printf '%s\n' "x86_64" ;;
    arm64|aarch64) printf '%s\n' "arm64" ;;
    *)
      echo "unsupported host arch: $(uname -m)" >&2
      exit 2
      ;;
  esac
}

cleanup() {
  rm -rf "${TMP_DIR}"
}
trap cleanup EXIT

MKDBG_INSTALL_DIR="${INSTALL_DIR}" bash "${ROOT_DIR}/tools/install_mkdbg.sh" > "${INSTALL_OUT}"
test -x "${INSTALL_DIR}/mkdbg"

"${INSTALL_DIR}/mkdbg" --version > "${VERSION_OUT}"

bash "${ROOT_DIR}/tools/build_mkdbg_native.sh" > /dev/null
MKDBG_INSTALL_DIR="${BINARY_INSTALL_DIR}" \
MKDBG_INSTALL_BINARY_PATH="${ROOT_DIR}/build/mkdbg-native" \
CC=missing-compiler \
bash "${ROOT_DIR}/tools/install_mkdbg.sh" > "${BINARY_INSTALL_OUT}"
test -x "${BINARY_INSTALL_DIR}/mkdbg"

"${BINARY_INSTALL_DIR}/mkdbg" --version > "${BINARY_VERSION_OUT}"

mkdir -p "${RELEASE_DIR}"
cp "${ROOT_DIR}/build/mkdbg-native-$(detect_host_os)-$(detect_host_arch)" "${RELEASE_DIR}/"
MKDBG_INSTALL_DIR="${AUTO_INSTALL_DIR}" \
MKDBG_INSTALL_BINARY_BASE_URL="file://${RELEASE_DIR}" \
CC=missing-compiler \
bash "${ROOT_DIR}/tools/install_mkdbg.sh" > "${AUTO_INSTALL_OUT}"
test -x "${AUTO_INSTALL_DIR}/mkdbg"

"${AUTO_INSTALL_DIR}/mkdbg" --version > "${AUTO_VERSION_OUT}"

python3 - "${INSTALL_OUT}" "${VERSION_OUT}" "${INSTALL_DIR}/mkdbg" \
  "${BINARY_INSTALL_OUT}" "${BINARY_VERSION_OUT}" "${BINARY_INSTALL_DIR}/mkdbg" \
  "${AUTO_INSTALL_OUT}" "${AUTO_VERSION_OUT}" "${AUTO_INSTALL_DIR}/mkdbg" <<'PY'
import sys
from pathlib import Path

install_text = Path(sys.argv[1]).read_text(encoding="utf-8")
version_text = Path(sys.argv[2]).read_text(encoding="utf-8")
target = Path(sys.argv[3])
binary_install_text = Path(sys.argv[4]).read_text(encoding="utf-8")
binary_version_text = Path(sys.argv[5]).read_text(encoding="utf-8")
binary_target = Path(sys.argv[6])
auto_install_text = Path(sys.argv[7]).read_text(encoding="utf-8")
auto_version_text = Path(sys.argv[8]).read_text(encoding="utf-8")
auto_target = Path(sys.argv[9])

if f"installed mkdbg (native-source) -> {target}" not in install_text:
    raise SystemExit(f"missing native-source install banner: {install_text!r}")
if "mkdbg-native 0.1.0" not in version_text:
    raise SystemExit(f"missing native version output: {version_text!r}")
if f"installed mkdbg (native-binary) -> {binary_target}" not in binary_install_text:
    raise SystemExit(f"missing native-binary install banner: {binary_install_text!r}")
if "mkdbg-native 0.1.0" not in binary_version_text:
    raise SystemExit(f"missing native binary version output: {binary_version_text!r}")
if f"installed mkdbg (native-binary) -> {auto_target}" not in auto_install_text:
    raise SystemExit(f"missing auto native-binary install banner: {auto_install_text!r}")
if "mkdbg-native 0.1.0" not in auto_version_text:
    raise SystemExit(f"missing auto native binary version output: {auto_version_text!r}")
PY

echo "install_mkdbg_host_tests: OK"
