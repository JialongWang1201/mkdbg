#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
INSTALL_DIR="${OVWATCH_INSTALL_DIR:-$HOME/.local/bin}"
TARGET="${INSTALL_DIR}/ovwatch"
LOCAL_SOURCE="${SCRIPT_DIR}/ovwatch"
REPO_SLUG="${OVWATCH_REPO_SLUG:-JialongWang1201/MicroKernel-MPU}"
REPO_REF="${OVWATCH_REF:-main}"
REMOTE_URL="https://raw.githubusercontent.com/${REPO_SLUG}/${REPO_REF}/tools/ovwatch"

mkdir -p "${INSTALL_DIR}"

if [[ -f "${LOCAL_SOURCE}" ]]; then
  cp "${LOCAL_SOURCE}" "${TARGET}"
else
  curl -fsSL "${REMOTE_URL}" -o "${TARGET}"
fi

chmod +x "${TARGET}"

echo "installed ovwatch -> ${TARGET}"
case ":${PATH}:" in
  *":${INSTALL_DIR}:"*) ;;
  *)
    echo "add ${INSTALL_DIR} to PATH to call ovwatch directly"
    ;;
esac
