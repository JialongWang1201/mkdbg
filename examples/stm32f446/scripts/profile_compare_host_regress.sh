#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
FIXTURE_DIR="${ROOT_DIR}/tests/fixtures/profile_compare"
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "${TMP_DIR}"' EXIT

BASELINE="${FIXTURE_DIR}/baseline.jsonl"
SAME="${FIXTURE_DIR}/candidate_same.jsonl"
REGRESSION="${FIXTURE_DIR}/candidate_regression.jsonl"

OUT_OK="${TMP_DIR}/profile_compare_ok.json"
OUT_REG="${TMP_DIR}/profile_compare_reg.json"

python3 "${ROOT_DIR}/tools/driver_profile_compare.py" \
  --baseline "${BASELINE}" \
  --candidate "${SAME}" \
  --driver uart \
  --fail-on-regression \
  --json > "${OUT_OK}"

python3 - "${OUT_OK}" <<'PY'
import json
import sys

report = json.load(open(sys.argv[1], "r", encoding="utf-8"))
if report.get("regression_count") != 0:
    raise SystemExit("expected no regression for candidate_same")
print("profile_compare_host_regress: same candidate OK")
PY

set +e
python3 "${ROOT_DIR}/tools/driver_profile_compare.py" \
  --baseline "${BASELINE}" \
  --candidate "${REGRESSION}" \
  --driver uart \
  --fail-on-regression \
  --json > "${OUT_REG}"
rc=$?
set -e
if [[ ${rc} -ne 1 ]]; then
  echo "expected exit code 1 for regression candidate, got ${rc}" >&2
  exit 1
fi

python3 - "${OUT_REG}" <<'PY'
import json
import sys

report = json.load(open(sys.argv[1], "r", encoding="utf-8"))
if report.get("regression_count", 0) < 1:
    raise SystemExit("expected regression_count >= 1 for regression fixture")
result = report["results"][0]
anomaly_types = {entry.get("type", "") for entry in result.get("anomalies", [])}
required = {"distribution_shift", "new_capability_usage", "state_jitter_increase"}
missing = sorted(required - anomaly_types)
if missing:
    raise SystemExit("missing anomaly types: " + ",".join(missing))
print("profile_compare_host_regress: regression candidate OK")
PY

echo "profile_compare_host_regress: OK"
