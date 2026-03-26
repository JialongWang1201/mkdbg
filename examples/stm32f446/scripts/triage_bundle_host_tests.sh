#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
OUT_JSON="${BUILD_DIR}/triage_bundle_host.json"
STDOUT_JSON="${BUILD_DIR}/triage_bundle_host.stdout.json"
REPLAY_JSON="${BUILD_DIR}/triage_bundle_host.replay.json"
FIXTURE="${ROOT_DIR}/tests/fixtures/triage/sample_snapshot.log"

mkdir -p "${BUILD_DIR}"

python3 "${ROOT_DIR}/tools/triage_bundle.py" \
  --source-log "${FIXTURE}" \
  --output "${OUT_JSON}" \
  --json > "${STDOUT_JSON}"

python3 - "${OUT_JSON}" "${STDOUT_JSON}" "${REPLAY_JSON}" <<'PY'
import json
import sys
from pathlib import Path

bundle_path = Path(sys.argv[1])
stdout_path = Path(sys.argv[2])
replay_path = Path(sys.argv[3])

bundle = json.loads(bundle_path.read_text(encoding="utf-8"))
summary = json.loads(stdout_path.read_text(encoding="utf-8"))
replay = json.loads(replay_path.read_text(encoding="utf-8"))

if not bundle.get("ok", False):
    raise SystemExit("bundle expected ok=true")
if summary.get("ok") is not True:
    raise SystemExit("stdout summary expected ok=true")
if summary.get("fault_slices", 0) < 1:
    raise SystemExit("expected at least one fault slice")
if summary.get("target_driver") != "sensor":
    raise SystemExit("expected target driver sensor")
if summary.get("replay_path") != str(replay_path):
    raise SystemExit("expected replay path in summary")
if bundle.get("replay_artifact", {}).get("path") != str(replay_path):
    raise SystemExit("bundle missing replay artifact path")
if bundle.get("replay_artifact", {}).get("sha256") != replay.get("replay_sha256"):
    raise SystemExit("bundle replay sha mismatch")
if replay.get("summary", {}).get("fault_slice_count") != 1:
    raise SystemExit("expected replay fault slice count")

fault_slices = bundle.get("snapshot", {}).get("fault_slices", [])
if len(fault_slices) < 1:
    raise SystemExit("missing parsed fault slices")

slice0 = fault_slices[0]
failure = slice0.get("failure", {})
if failure.get("category") != "early_resource_fault":
    raise SystemExit("failure category parse mismatch")
feature = slice0.get("feature_vector", {})
if feature.get("dma_full") != 1:
    raise SystemExit("feature vector parse mismatch")
hyp = slice0.get("hypotheses", [])
if len(hyp) < 1 or hyp[0].get("name") != "dma_backpressure":
    raise SystemExit("hypothesis parse mismatch")

artifacts = bundle.get("artifact_logs", {})
if "snapshot" not in artifacts:
    raise SystemExit("expected snapshot raw artifact log")

print("triage_bundle_host_tests: OK")
PY
