#!/usr/bin/env python3
import argparse
import json
import time
from pathlib import Path
from typing import Any

from triage_bundle import build_bundle_log
from triage_replay_lib import build_diff, build_replay


ROOT = Path(__file__).resolve().parent.parent
LOG_DIR = ROOT / "logs"


def load_json_object(path: Path) -> dict[str, Any]:
    obj = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(obj, dict):
        raise ValueError(f"json root must be object: {path}")
    return obj


def build_replay_from_args(bundle_json: str | None, source_log: str | None, target_driver: str | None) -> dict[str, Any]:
    if (bundle_json is None and source_log is None) or (bundle_json is not None and source_log is not None):
        raise ValueError("specify exactly one of --bundle-json or --source-log")

    if bundle_json is not None:
        path = Path(bundle_json)
        if not path.is_absolute():
            path = ROOT / path
        bundle = load_json_object(path)
    else:
        log_path = Path(source_log)
        if not log_path.is_absolute():
            log_path = ROOT / log_path
        bundle, _ = build_bundle_log(log_path, target_driver)
    return build_replay(bundle)


def _default_output(stem: str) -> Path:
    LOG_DIR.mkdir(parents=True, exist_ok=True)
    ts = time.strftime("%Y%m%d_%H%M%S")
    return LOG_DIR / f"{stem}_{ts}.json"


def _resolve_output(path_text: str | None, stem: str) -> Path:
    path = Path(path_text) if path_text else _default_output(stem)
    if not path.is_absolute():
        path = ROOT / path
    path.parent.mkdir(parents=True, exist_ok=True)
    return path


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Build deterministic replay artifacts from triage bundles and diff them."
    )
    sub = ap.add_subparsers(dest="cmd", required=True)

    p_build = sub.add_parser("build", help="build canonical replay json from bundle/log input")
    p_build.add_argument("--bundle-json", help="existing triage bundle json path")
    p_build.add_argument("--source-log", help="existing snapshot log path")
    p_build.add_argument("--target-driver", help="override target driver when using --source-log")
    p_build.add_argument("--output", help="output replay json path")
    p_build.add_argument("--json", action="store_true", help="print machine-readable summary")

    p_diff = sub.add_parser("diff", help="diff two canonical replay json files")
    p_diff.add_argument("--baseline", required=True, help="baseline replay json path")
    p_diff.add_argument("--candidate", required=True, help="candidate replay json path")
    p_diff.add_argument("--output", help="output diff json path")
    p_diff.add_argument("--fail-on-diff", action="store_true", help="return non-zero when differences are found")
    p_diff.add_argument("--json", action="store_true", help="print machine-readable summary")

    ns = ap.parse_args()

    try:
        if ns.cmd == "build":
            replay = build_replay_from_args(ns.bundle_json, ns.source_log, ns.target_driver)
            out_path = _resolve_output(ns.output, "triage_replay")
            out_path.write_text(json.dumps(replay, ensure_ascii=True, indent=2) + "\n", encoding="utf-8")
            if ns.json:
                print(
                    json.dumps(
                        {
                            "ok": True,
                            "replay_path": str(out_path),
                            "fault_slices": replay["summary"]["fault_slice_count"],
                            "target_driver": replay["summary"]["target_driver"],
                            "replay_sha256": replay["replay_sha256"],
                        },
                        ensure_ascii=True,
                    )
                )
            else:
                print(f"replay: {out_path}")
                print(f"fault_slices: {replay['summary']['fault_slice_count']}")
                print(f"target_driver: {replay['summary']['target_driver']}")
                print(f"replay_sha256: {replay['replay_sha256']}")
            return 0

        baseline_path = Path(ns.baseline)
        candidate_path = Path(ns.candidate)
        if not baseline_path.is_absolute():
            baseline_path = ROOT / baseline_path
        if not candidate_path.is_absolute():
            candidate_path = ROOT / candidate_path

        baseline = load_json_object(baseline_path)
        candidate = load_json_object(candidate_path)
        diff = build_diff(baseline, candidate)
        out_path = _resolve_output(ns.output, "triage_replay_diff")
        out_path.write_text(json.dumps(diff, ensure_ascii=True, indent=2) + "\n", encoding="utf-8")
        if ns.json:
            print(
                json.dumps(
                    {
                        "ok": True,
                        "same": diff["same"],
                        "change_count": diff["change_count"],
                        "diff_path": str(out_path),
                    },
                    ensure_ascii=True,
                )
            )
        else:
            print(f"diff: {out_path}")
            print(f"same: {diff['same']}")
            print(f"change_count: {diff['change_count']}")
        if ns.fail_on_diff and not diff["same"]:
            return 1
        return 0
    except Exception as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
