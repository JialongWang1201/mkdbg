#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "${TMP_DIR}"' EXIT

HELLO_C="${ROOT_DIR}/demo/hello.c"
ASM_OUT="${TMP_DIR}/hello.asm"
BIN_OUT="${TMP_DIR}/hello.bin"

"${ROOT_DIR}/tools/vm32" tinyc "${HELLO_C}" -o "${ASM_OUT}"
"${ROOT_DIR}/tools/vm32" asm "${ASM_OUT}" -o "${BIN_OUT}"
"${ROOT_DIR}/tools/vm32" disasm "${BIN_OUT}" --base 0 > /dev/null
"${ROOT_DIR}/tools/vm32" load "${BIN_OUT}" --dry-run > /dev/null

echo "vm32_ci: OK"
