#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"

ASM_OUT="$ROOT/demo/hello.asm"
BIN_OUT="$ROOT/demo/hello.bin"

python3 "$ROOT/tools/vm32_tinyc.py" "$ROOT/demo/hello.c" -o "$ASM_OUT"
python3 "$ROOT/tools/vm32_asm.py" "$ASM_OUT" -o "$BIN_OUT"
python3 "$ROOT/tools/vm32_load.py" "$BIN_OUT" --dry-run
