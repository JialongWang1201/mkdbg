#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
EXAMPLE_DIR="${ROOT_DIR}/examples/stm32f446"
HEADER_OUT="${BUILD_DIR}/bringup_manifest_gen.h"
JSON_OUT="${BUILD_DIR}/bringup_manifest.export.json"
DOC_OUT="${BUILD_DIR}/bringup_manifest.md"

mkdir -p "${BUILD_DIR}"

python3 "${ROOT_DIR}/tools/bringup_compile.py" \
  --header-out "${HEADER_OUT}" \
  --markdown-out "${DOC_OUT}" \
  --json-out "${JSON_OUT}" \
  --json > /dev/null

cmp -s "${HEADER_OUT}" "${EXAMPLE_DIR}/include/bringup_manifest_gen.h"
cmp -s "${DOC_OUT}" "${ROOT_DIR}/docs/generated/bringup_manifest.md"

python3 - "${JSON_OUT}" "${DOC_OUT}" <<'PY'
import json
import sys
from pathlib import Path

obj = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
if obj.get("phase_count") != 9:
    raise SystemExit("expected phase_count=9")
if obj.get("stage_count") != 5:
    raise SystemExit("expected stage_count=5")
if obj.get("driver_edge_count") < 5:
    raise SystemExit("expected at least 5 driver edges")
if obj.get("resource_edge_count") < 8:
    raise SystemExit("expected at least 8 resource edges")
phases = obj.get("phases", [])
if not isinstance(phases, list) or phases[0].get("name") != "rom-early-init":
    raise SystemExit("unexpected first phase")
stages = obj.get("stages", [])
if not isinstance(stages, list) or stages[3].get("name") != "drivers":
    raise SystemExit("unexpected drivers stage")
doc = Path(sys.argv[2]).read_text(encoding="utf-8")
if "## Phases" not in doc:
    raise SystemExit("expected phases section in markdown export")
if "| BRINGUP_PHASE_ROM_EARLY_INIT | rom-early-init | BRINGUP_STAGE_INIT | rom, early |" not in doc:
    raise SystemExit("expected generated phase row in markdown export")
print("bringup_compile_host_tests: OK")
PY
