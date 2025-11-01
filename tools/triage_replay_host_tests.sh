#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
REPLAY_JSON="${BUILD_DIR}/triage_replay_host.json"
REPLAY_STDOUT="${BUILD_DIR}/triage_replay_host.stdout.json"
LOG_REPLAY_JSON="${BUILD_DIR}/triage_replay_log_host.json"
DIFF_JSON="${BUILD_DIR}/triage_replay_diff_host.json"
DIFF_STDOUT="${BUILD_DIR}/triage_replay_diff_host.stdout.json"
CANDIDATE_JSON="${BUILD_DIR}/triage_replay_candidate.json"
BUNDLE_FIXTURE="${ROOT_DIR}/tests/fixtures/triage/sample_bundle.json"
LOG_FIXTURE="${ROOT_DIR}/tests/fixtures/triage/sample_snapshot.log"

mkdir -p "${BUILD_DIR}"

python3 "${ROOT_DIR}/tools/triage_replay.py" build \
  --bundle-json "${BUNDLE_FIXTURE}" \
  --output "${REPLAY_JSON}" \
  --json > "${REPLAY_STDOUT}"

python3 "${ROOT_DIR}/tools/triage_replay.py" build \
  --source-log "${LOG_FIXTURE}" \
  --target-driver sensor \
  --output "${LOG_REPLAY_JSON}" \
  --json > /dev/null

python3 - "${REPLAY_JSON}" "${REPLAY_STDOUT}" "${LOG_REPLAY_JSON}" "${CANDIDATE_JSON}" <<'PY'
import json
import sys
from pathlib import Path

replay_path = Path(sys.argv[1])
stdout_path = Path(sys.argv[2])
log_replay_path = Path(sys.argv[3])
candidate_path = Path(sys.argv[4])

replay = json.loads(replay_path.read_text(encoding="utf-8"))
summary = json.loads(stdout_path.read_text(encoding="utf-8"))
log_replay = json.loads(log_replay_path.read_text(encoding="utf-8"))

if replay.get("schema") != "triage-replay/v1":
    raise SystemExit("unexpected replay schema")
if summary.get("ok") is not True:
    raise SystemExit("expected replay build ok=true")
if replay.get("summary", {}).get("target_driver") != "sensor":
    raise SystemExit("expected target driver sensor")
if replay.get("summary", {}).get("fault_slice_count") != 1:
    raise SystemExit("expected one fault slice")
if replay.get("fault_slices", [])[0].get("failure", {}).get("category") != "early_resource_fault":
    raise SystemExit("unexpected failure category")
events = replay.get("fault_slices", [])[0].get("events", [])
if len(events) < 1 or "bringup stage drivers enter" not in events[0].get("signature", ""):
    raise SystemExit("expected normalized event signature")
if log_replay.get("summary", {}).get("fault_slice_count") != 1:
    raise SystemExit("log replay expected one fault slice")

candidate = json.loads(json.dumps(replay))
candidate["summary"]["target_driver"] = "uart"
candidate["fault_slices"][0]["failure"]["category"] = "ordering_violation"
candidate_path.write_text(json.dumps(candidate, ensure_ascii=True, indent=2) + "\n", encoding="utf-8")
PY

python3 "${ROOT_DIR}/tools/triage_replay.py" diff \
  --baseline "${REPLAY_JSON}" \
  --candidate "${CANDIDATE_JSON}" \
  --output "${DIFF_JSON}" \
  --json > "${DIFF_STDOUT}"

python3 - "${DIFF_JSON}" "${DIFF_STDOUT}" <<'PY'
import json
import sys
from pathlib import Path

diff_path = Path(sys.argv[1])
stdout_path = Path(sys.argv[2])

diff = json.loads(diff_path.read_text(encoding="utf-8"))
summary = json.loads(stdout_path.read_text(encoding="utf-8"))

if diff.get("schema") != "triage-replay-diff/v1":
    raise SystemExit("unexpected diff schema")
if diff.get("same") is not False:
    raise SystemExit("expected replay diff to detect changes")
if summary.get("change_count", 0) < 2:
    raise SystemExit("expected at least two replay changes")

paths = {item.get("path") for item in diff.get("changes", [])}
if "summary.target_driver" not in paths:
    raise SystemExit("missing target_driver diff")
if "fault_slices[0].failure.category" not in paths:
    raise SystemExit("missing failure category diff")

print("triage_replay_host_tests: OK")
PY
