#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
FIXTURE="${ROOT_DIR}/tests/fixtures/triage/sample_bundle.json"
RENDER_OUT="${BUILD_DIR}/bringup_ui.render.txt"

mkdir -p "${BUILD_DIR}"

python3 -m py_compile "${ROOT_DIR}/tools/bringup_ui.py"
python3 "${ROOT_DIR}/tools/bringup_ui.py" \
  --bundle-json "${FIXTURE}" \
  --render-once \
  --auto-refresh-s 5 \
  --width 120 \
  --height 38 > "${RENDER_OUT}"

python3 - "${RENDER_OUT}" <<'PY'
import sys
from pathlib import Path

text = Path(sys.argv[1]).read_text(encoding="utf-8")
checks = [
    "MicroKernel-MPU Bringup Console",
    "Hardware-first terminal dashboard for bringup, dependency, and fault triage",
    "failure=early_resource_fault",
    "hypothesis=dma_backpressure",
    "impact drivers:",
    "sensor",
    "auto=5s",
    "age=",
    "g refresh a auto +/- rate 1/2/3 [ ] q",
]
for item in checks:
    if item not in text:
        raise SystemExit(f"missing expected render text: {item}")
print("bringup_ui_host_tests: OK")
PY

"${ROOT_DIR}/tools/vm32" bringup-ui --bundle-json "${FIXTURE}" --render-once --auto-refresh-s 10 --width 100 --height 32 > /dev/null
