#!/usr/bin/env python3
import argparse
import json
import subprocess
import sys
import time
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parent.parent
TOOLS = ROOT / "tools"


def run_json_cmd(cmd: list[str]) -> dict[str, Any]:
    proc = subprocess.run(cmd, capture_output=True, text=True, cwd=str(ROOT))
    if proc.returncode != 0:
        detail = (proc.stdout + "\n" + proc.stderr).strip()
        raise RuntimeError(f"command failed rc={proc.returncode}: {' '.join(cmd)}\n{detail}")
    text = proc.stdout.strip()
    if text == "":
        raise RuntimeError(f"command returned empty stdout: {' '.join(cmd)}")
    try:
        obj = json.loads(text)
    except Exception as exc:
        raise RuntimeError(f"invalid JSON from command: {' '.join(cmd)}") from exc
    if not isinstance(obj, dict):
        raise RuntimeError(f"JSON root is not object: {' '.join(cmd)}")
    return obj


def load_json(path: Path) -> dict[str, Any]:
    obj = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(obj, dict):
        raise RuntimeError(f"json root is not object: {path}")
    return obj


def summarize_profile(report: dict[str, Any]) -> dict[str, Any]:
    out_drivers: list[dict[str, Any]] = []
    for row in report.get("results", []):
        if not isinstance(row, dict):
            continue
        anomalies = row.get("anomalies", [])
        anomaly_types: list[str] = []
        if isinstance(anomalies, list):
            for item in anomalies:
                if isinstance(item, dict):
                    anomaly_types.append(str(item.get("type", "unknown")))
        out_drivers.append(
            {
                "driver": row.get("driver"),
                "regression": bool(row.get("regression", False)),
                "confidence": int(row.get("confidence", 0)),
                "anomaly_types": anomaly_types,
                "why_regression": row.get("regression_explain", ""),
            }
        )

    return {
        "regression_count": int(report.get("regression_count", 0)),
        "driver_count": int(report.get("driver_count", len(out_drivers))),
        "drivers": out_drivers,
    }


def summarize_faults(bundle: dict[str, Any]) -> list[dict[str, Any]]:
    out: list[dict[str, Any]] = []
    snapshot = bundle.get("snapshot", {})
    if not isinstance(snapshot, dict):
        return out

    fault_slices = snapshot.get("fault_slices", [])
    if not isinstance(fault_slices, list):
        return out

    for row in fault_slices:
        if not isinstance(row, dict):
            continue
        failure = row.get("failure", {})
        meta = row.get("meta", {})
        explain = row.get("failure_explain", {})
        checkpoints = row.get("ai_checkpoints", [])
        events = row.get("events", [])

        stage = "none"
        if isinstance(failure, dict) and isinstance(failure.get("stage"), str):
            stage = str(failure.get("stage"))
        elif isinstance(meta, dict) and isinstance(meta.get("stage"), str):
            stage = str(meta.get("stage"))

        supporting_stage_set: set[str] = set()
        if isinstance(events, list):
            for event in events:
                if isinstance(event, dict):
                    stage_name = event.get("stage")
                    if isinstance(stage_name, str) and stage_name != "":
                        supporting_stage_set.add(stage_name)

        checkpoint_cmds: list[str] = []
        if isinstance(checkpoints, list):
            for cp in checkpoints:
                if isinstance(cp, dict):
                    cmd = cp.get("cmd")
                    if isinstance(cmd, str) and cmd != "":
                        checkpoint_cmds.append(cmd)

        why_parts: list[str] = []
        if isinstance(explain, dict):
            p1 = explain.get("p1")
            p2 = explain.get("p2")
            if isinstance(p1, str) and p1 != "":
                why_parts.append(p1)
            if isinstance(p2, str) and p2 != "":
                why_parts.append(p2)

        out.append(
            {
                "slice_id": int(row.get("slice_id", 0)),
                "failure_class": failure.get("category", "unknown"),
                "stage": stage,
                "supporting_event_ids": failure.get("evidence_ids", []),
                "supporting_stages": sorted(supporting_stage_set),
                "recommended_checkpoints": checkpoint_cmds,
                "why_regression": " ".join(why_parts).strip(),
            }
        )

    return out


def main() -> int:
    ap = argparse.ArgumentParser(
        description=(
            "Generate unified regression_summary.json with failure class, evidence event IDs, and regression reasons."
        )
    )
    ap.add_argument(
        "--profile-baseline",
        default="tests/fixtures/profile_compare/baseline.jsonl",
        help="baseline profile log/jsonl",
    )
    ap.add_argument(
        "--profile-candidate",
        default="tests/fixtures/profile_compare/candidate_regression.jsonl",
        help="candidate profile log/jsonl",
    )
    ap.add_argument(
        "--triage-source-log",
        default="tests/fixtures/triage/sample_snapshot.log",
        help="snapshot/bringup log for triage parsing",
    )
    ap.add_argument(
        "--output",
        default="build/regression_summary.json",
        help="output summary path",
    )
    ap.add_argument(
        "--json",
        action="store_true",
        help="print compact machine-readable result",
    )
    ap.add_argument(
        "--fail-on-regression",
        action="store_true",
        help="return non-zero when any profile regression is present",
    )
    ns = ap.parse_args()

    profile_baseline = Path(ns.profile_baseline)
    profile_candidate = Path(ns.profile_candidate)
    triage_source_log = Path(ns.triage_source_log)
    output_path = Path(ns.output)

    if not profile_baseline.is_absolute():
        profile_baseline = ROOT / profile_baseline
    if not profile_candidate.is_absolute():
        profile_candidate = ROOT / profile_candidate
    if not triage_source_log.is_absolute():
        triage_source_log = ROOT / triage_source_log
    if not output_path.is_absolute():
        output_path = ROOT / output_path
    output_path.parent.mkdir(parents=True, exist_ok=True)

    triage_bundle_path = output_path.with_suffix(".triage_bundle.json")

    try:
        profile_report = run_json_cmd(
            [
                sys.executable,
                str(TOOLS / "driver_profile_compare.py"),
                "--baseline",
                str(profile_baseline),
                "--candidate",
                str(profile_candidate),
                "--driver",
                "uart",
                "--json",
            ]
        )

        _ = run_json_cmd(
            [
                sys.executable,
                str(TOOLS / "triage_bundle.py"),
                "--source-log",
                str(triage_source_log),
                "--output",
                str(triage_bundle_path),
                "--json",
            ]
        )
        triage_bundle = load_json(triage_bundle_path)
    except Exception as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    profile_summary = summarize_profile(profile_report)
    fault_summary = summarize_faults(triage_bundle)
    regression_count = int(profile_summary.get("regression_count", 0))

    summary = {
        "ok": regression_count == 0,
        "generated_at": time.strftime("%Y-%m-%d %H:%M:%S"),
        "inputs": {
            "profile_baseline": str(profile_baseline),
            "profile_candidate": str(profile_candidate),
            "triage_source_log": str(triage_source_log),
        },
        "profile": profile_summary,
        "fault_explanations": fault_summary,
    }

    output_path.write_text(json.dumps(summary, ensure_ascii=True, indent=2) + "\n", encoding="utf-8")

    if ns.json:
        print(
            json.dumps(
                {
                    "ok": summary["ok"],
                    "output": str(output_path),
                    "regression_count": regression_count,
                    "fault_slice_count": len(fault_summary),
                },
                ensure_ascii=True,
            )
        )
    else:
        print(f"summary: {output_path}")
        print(f"regression_count: {regression_count}")
        print(f"fault_slice_count: {len(fault_summary)}")
        print(f"overall: {'PASS' if summary['ok'] else 'REGRESSION'}")

    if ns.fail_on_regression and regression_count != 0:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
