#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TMP_DIR="$(mktemp -d)"
INSTALL_DIR="${TMP_DIR}/install"
INSTALL_OUT="${TMP_DIR}/install.out"
VERSION_OUT="${TMP_DIR}/version.out"

cleanup() {
  rm -rf "${TMP_DIR}"
}
trap cleanup EXIT

MKDBG_INSTALL_DIR="${INSTALL_DIR}" bash "${ROOT_DIR}/tools/install_mkdbg.sh" > "${INSTALL_OUT}"
test -x "${INSTALL_DIR}/mkdbg"

"${INSTALL_DIR}/mkdbg" --version > "${VERSION_OUT}"

python3 - "${INSTALL_OUT}" "${VERSION_OUT}" "${INSTALL_DIR}/mkdbg" <<'PY'
import sys
from pathlib import Path

install_text = Path(sys.argv[1]).read_text(encoding="utf-8")
version_text = Path(sys.argv[2]).read_text(encoding="utf-8")
target = Path(sys.argv[3])

if f"installed mkdbg (native) -> {target}" not in install_text:
    raise SystemExit(f"missing native install banner: {install_text!r}")
if "mkdbg-native 0.1.0" not in version_text:
    raise SystemExit(f"missing native version output: {version_text!r}")
PY

echo "install_mkdbg_host_tests: OK"
