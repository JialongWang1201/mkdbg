#!/usr/bin/env bash
set -euo pipefail

INSTALL_DIR="${MKDBG_INSTALL_DIR:-$HOME/.local/bin}"
TARGET="${INSTALL_DIR}/mkdbg"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LOCAL_SOURCE="${SCRIPT_DIR}/mkdbg"
REPO_SLUG="${MKDBG_REPO_SLUG:-JialongWang1201/MicroKernel-MPU}"
REPO_REF="${MKDBG_REF:-main}"
REMOTE_URL="https://raw.githubusercontent.com/${REPO_SLUG}/${REPO_REF}/tools/mkdbg"

mkdir -p "${INSTALL_DIR}"

if [[ -f "${LOCAL_SOURCE}" ]]; then
  cp "${LOCAL_SOURCE}" "${TARGET}"
else
  if ! command -v curl >/dev/null 2>&1; then
    echo "error: curl is required when installing without a local checkout" >&2
    exit 2
  fi
  curl -fsSL "${REMOTE_URL}" -o "${TARGET}"
fi

chmod +x "${TARGET}"

echo "installed mkdbg -> ${TARGET}"
case ":${PATH}:" in
  *:"${INSTALL_DIR}":*)
    ;;
  *)
    echo "add ${INSTALL_DIR} to PATH to call mkdbg directly"
    ;;
esac
