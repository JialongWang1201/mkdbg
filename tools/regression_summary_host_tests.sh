#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
OUT_JSON="${BUILD_DIR}/regression_summary.json"
STDOUT_JSON="${BUILD_DIR}/regression_summary.stdout.json"

mkdir -p "${BUILD_DIR}"

python3 "${ROOT_DIR}/tools/regression_summary.py" \
  --output "${OUT_JSON}" \
  --json > "${STDOUT_JSON}"

python3 - "${OUT_JSON}" "${STDOUT_JSON}" <<'PY'
import json
import sys
from pathlib import Path

summary_path = Path(sys.argv[1])
stdout_path = Path(sys.argv[2])

summary = json.loads(summary_path.read_text(encoding="utf-8"))
meta = json.loads(stdout_path.read_text(encoding="utf-8"))

if summary.get("profile", {}).get("regression_count", 0) < 1:
    raise SystemExit("expected profile regression_count >= 1")
if meta.get("regression_count", 0) < 1:
    raise SystemExit("stdout meta regression_count mismatch")

faults = summary.get("fault_explanations", [])
if len(faults) < 1:
    raise SystemExit("expected at least one fault explanation")

f0 = faults[0]
if not f0.get("failure_class"):
    raise SystemExit("missing failure_class")
if not isinstance(f0.get("supporting_event_ids"), list):
    raise SystemExit("supporting_event_ids must be a list")
if "why_regression" not in f0:
    raise SystemExit("missing why_regression")

drivers = summary.get("profile", {}).get("drivers", [])
if len(drivers) < 1:
    raise SystemExit("expected at least one profile driver summary")
if "why_regression" not in drivers[0]:
    raise SystemExit("missing profile why_regression")

print("regression_summary_host_tests: OK")
PY
