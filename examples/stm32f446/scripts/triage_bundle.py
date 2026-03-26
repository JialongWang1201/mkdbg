#!/usr/bin/env python3
import argparse
import json
import re
import subprocess
import sys
import time
from pathlib import Path
from typing import Any
from triage_replay_lib import build_replay

try:
    import serial  # type: ignore
except Exception:
    serial = None


ROOT = Path(__file__).resolve().parent.parent.parent.parent
LOG_DIR = ROOT / "logs"

SLICE_RE = re.compile(
    r"^snapshot slice id=(\d+) reason=([^ ]+) corr_id=(0x[0-9A-Fa-f]+) "
    r"stage=([^ ]+) events=(\d+) fault=(\d+) reset=(\d+) age_ms=\[(\d+)\.\.(\d+)\]$"
)
FEATURE_RE = re.compile(
    r"^snapshot feature slice=(\d+) drv=([^ ]+) irq_abn=(\d+) dma_full=(\d+) dma_idle=(\d+) "
    r"kdi_fail_freq=(\d+) state_loop=(\d+) eperm=(\d+) mpu_vio=(\d+)$"
)
FEATURE_DETAIL_RE = re.compile(
    r"^snapshot feature detail slice=(\d+) irq_evt=(\d+) irq_rate=(\d+) kdi_fail=(\d+) "
    r"lookback_ms=(\d+) corr=(0x[0-9A-Fa-f]+)$"
)
HYP_RE = re.compile(
    r"^snapshot hypothesis slice=(\d+) name=([a-z_]+) conf=(\d+) evidence=([0-9,]+|none)$"
)
HYP_EXPL_RE = re.compile(
    r"^snapshot hypothesis explain slice=(\d+) name=([a-z_]+) p([12])=(.*)$"
)
FAIL_RE = re.compile(
    r"^snapshot failure slice=(\d+) category=([a-z_]+) conf=(\d+) evidence=([0-9,]+|none) stage=([^ ]+)$"
)
FAIL_EXPL_RE = re.compile(
    r"^snapshot failure explain slice=(\d+) category=([a-z_]+) p([12])=(.*)$"
)
AI_FAIL_RE = re.compile(
    r"^snapshot ai failure slice=(\d+) class=([a-z_]+) stage=([^ ]+) driver=([^ ]+) evidence=([0-9,]+|none)$"
)
AI_SUPPORT_RE = re.compile(
    r"^snapshot ai support slice=(\d+) event=(\d+) stage=([^ ]+) msg=(.*)$"
)
AI_CHECK_RE = re.compile(
    r"^snapshot ai checkpoint slice=(\d+) step=(\d+) cmd=([^ ]+) focus=(.*)$"
)
EVENT_RE = re.compile(
    r"^snapshot event slice=(\d+) id=(\d+) age_ms=(\d+) ts_ms=(\d+) "
    r"corr_id=(0x[0-9A-Fa-f]+) flags=([^ ]+) stage=([^ ]+) msg=(.*)$"
)
BRINGUP_RE = re.compile(
    r"^snapshot bringup boot_complete=(\d+) active=([^ ]+) last_error=([-0-9]+)$"
)
EVENTS_RE = re.compile(r"^snapshot events window_ms=(\d+) count=(\d+)$")
SLICES_RE = re.compile(r"^snapshot slices count=(\d+)$")


def normalize_text(text: str) -> str:
    return text.replace("\r\n", "\n").replace("\r", "\n")


def parse_evidence_ids(raw: str) -> list[int]:
    raw = raw.strip()
    if raw == "" or raw == "none":
        return []
    out: list[int] = []
    for item in raw.split(","):
        item = item.strip()
        if item == "":
            continue
        try:
            out.append(int(item, 10))
        except Exception:
            continue
    return out


def parse_json_lines(text: str) -> list[dict[str, Any]]:
    out: list[dict[str, Any]] = []
    for line in normalize_text(text).splitlines():
        row = line.strip()
        if not row.startswith("{"):
            continue
        try:
            obj = json.loads(row)
        except Exception:
            continue
        if isinstance(obj, dict):
            out.append(obj)
    return out


def dep_parse_impact(rows: list[dict[str, Any]]) -> dict[str, Any]:
    summary: dict[str, Any] | None = None
    drivers: list[dict[str, Any]] = []
    for row in rows:
        row_type = str(row.get("type", ""))
        if row_type == "dep-impact-summary":
            summary = row
        elif row_type == "dep-impact":
            drivers.append(row)
    return {
        "summary": summary if summary is not None else {},
        "drivers": drivers,
    }


def dep_parse_whatif(rows: list[dict[str, Any]]) -> dict[str, Any]:
    summary: dict[str, Any] | None = None
    drivers: list[dict[str, Any]] = []
    stages: list[dict[str, Any]] = []
    secondary: list[dict[str, Any]] = []
    for row in rows:
        row_type = str(row.get("type", ""))
        if row_type == "dep-whatif-summary":
            summary = row
        elif row_type == "dep-whatif-driver":
            drivers.append(row)
        elif row_type == "dep-whatif-stage":
            stages.append(row)
        elif row_type == "dep-whatif-secondary":
            secondary.append(row)
    return {
        "summary": summary if summary is not None else {},
        "drivers": drivers,
        "stages": stages,
        "secondary": secondary,
    }


def ensure_slice(map_by_id: dict[int, dict[str, Any]], slice_id: int) -> dict[str, Any]:
    if slice_id not in map_by_id:
        map_by_id[slice_id] = {
            "slice_id": slice_id,
            "meta": {},
            "feature_vector": {},
            "feature_detail": {},
            "failure": {},
            "failure_explain": {},
            "ai_failure": {},
            "hypotheses": [],
            "ai_support": [],
            "ai_checkpoints": [],
            "events": [],
        }
    return map_by_id[slice_id]


def parse_snapshot_text(text: str) -> dict[str, Any]:
    lines = [ln.strip() for ln in normalize_text(text).splitlines() if ln.strip() != ""]
    snapshot_lines = [ln for ln in lines if ln.startswith("snapshot ")]

    out: dict[str, Any] = {
        "line_count": len(snapshot_lines),
        "bringup": {},
        "events_window": {},
        "slice_count_declared": 0,
        "fault_slices": [],
    }

    hyp_index: dict[tuple[int, str], dict[str, Any]] = {}
    slices: dict[int, dict[str, Any]] = {}

    for line in snapshot_lines:
        m = BRINGUP_RE.match(line)
        if m is not None:
            out["bringup"] = {
                "boot_complete": int(m.group(1), 10),
                "active": m.group(2),
                "last_error": int(m.group(3), 10),
            }
            continue

        m = EVENTS_RE.match(line)
        if m is not None:
            out["events_window"] = {
                "window_ms": int(m.group(1), 10),
                "count": int(m.group(2), 10),
            }
            continue

        m = SLICES_RE.match(line)
        if m is not None:
            out["slice_count_declared"] = int(m.group(1), 10)
            continue

        m = SLICE_RE.match(line)
        if m is not None:
            slice_id = int(m.group(1), 10)
            slice_obj = ensure_slice(slices, slice_id)
            slice_obj["meta"] = {
                "reason": m.group(2),
                "corr_id": m.group(3),
                "stage": m.group(4),
                "events": int(m.group(5), 10),
                "fault_events": int(m.group(6), 10),
                "reset_events": int(m.group(7), 10),
                "age_ms_min": int(m.group(8), 10),
                "age_ms_max": int(m.group(9), 10),
            }
            continue

        m = FEATURE_RE.match(line)
        if m is not None:
            slice_id = int(m.group(1), 10)
            slice_obj = ensure_slice(slices, slice_id)
            slice_obj["feature_vector"] = {
                "driver": m.group(2),
                "irq_abnormal": int(m.group(3), 10),
                "dma_full": int(m.group(4), 10),
                "dma_idle_spin": int(m.group(5), 10),
                "kdi_fail_frequent": int(m.group(6), 10),
                "state_error_reset_loop": int(m.group(7), 10),
                "cap_eperm": int(m.group(8), 10),
                "cap_mpu_violation": int(m.group(9), 10),
            }
            continue

        m = FEATURE_DETAIL_RE.match(line)
        if m is not None:
            slice_id = int(m.group(1), 10)
            slice_obj = ensure_slice(slices, slice_id)
            slice_obj["feature_detail"] = {
                "irq_event_count": int(m.group(2), 10),
                "irq_rate_per_sec": int(m.group(3), 10),
                "kdi_fail_count": int(m.group(4), 10),
                "lookback_ms": int(m.group(5), 10),
                "corr_id": m.group(6),
            }
            continue

        m = HYP_RE.match(line)
        if m is not None:
            slice_id = int(m.group(1), 10)
            name = m.group(2)
            slice_obj = ensure_slice(slices, slice_id)
            hyp = {
                "name": name,
                "confidence": int(m.group(3), 10),
                "evidence_ids": parse_evidence_ids(m.group(4)),
                "explain_p1": "",
                "explain_p2": "",
            }
            slice_obj["hypotheses"].append(hyp)
            hyp_index[(slice_id, name)] = hyp
            continue

        m = HYP_EXPL_RE.match(line)
        if m is not None:
            slice_id = int(m.group(1), 10)
            name = m.group(2)
            which = m.group(3)
            value = m.group(4)
            hyp = hyp_index.get((slice_id, name))
            if hyp is not None:
                key = "explain_p1" if which == "1" else "explain_p2"
                hyp[key] = value
            continue

        m = FAIL_RE.match(line)
        if m is not None:
            slice_id = int(m.group(1), 10)
            slice_obj = ensure_slice(slices, slice_id)
            slice_obj["failure"] = {
                "category": m.group(2),
                "confidence": int(m.group(3), 10),
                "evidence_ids": parse_evidence_ids(m.group(4)),
                "stage": m.group(5),
            }
            continue

        m = FAIL_EXPL_RE.match(line)
        if m is not None:
            slice_id = int(m.group(1), 10)
            which = m.group(3)
            value = m.group(4)
            slice_obj = ensure_slice(slices, slice_id)
            key = "p1" if which == "1" else "p2"
            slice_obj["failure_explain"][key] = value
            continue

        m = AI_FAIL_RE.match(line)
        if m is not None:
            slice_id = int(m.group(1), 10)
            slice_obj = ensure_slice(slices, slice_id)
            slice_obj["ai_failure"] = {
                "class": m.group(2),
                "stage": m.group(3),
                "driver": m.group(4),
                "evidence_ids": parse_evidence_ids(m.group(5)),
            }
            continue

        m = AI_SUPPORT_RE.match(line)
        if m is not None:
            slice_id = int(m.group(1), 10)
            slice_obj = ensure_slice(slices, slice_id)
            slice_obj["ai_support"].append(
                {
                    "event_id": int(m.group(2), 10),
                    "stage": m.group(3),
                    "msg": m.group(4),
                }
            )
            continue

        m = AI_CHECK_RE.match(line)
        if m is not None:
            slice_id = int(m.group(1), 10)
            slice_obj = ensure_slice(slices, slice_id)
            slice_obj["ai_checkpoints"].append(
                {
                    "step": int(m.group(2), 10),
                    "cmd": m.group(3),
                    "focus": m.group(4),
                }
            )
            continue

        m = EVENT_RE.match(line)
        if m is not None:
            slice_id = int(m.group(1), 10)
            slice_obj = ensure_slice(slices, slice_id)
            slice_obj["events"].append(
                {
                    "event_id": int(m.group(2), 10),
                    "age_ms": int(m.group(3), 10),
                    "ts_ms": int(m.group(4), 10),
                    "corr_id": m.group(5),
                    "flags": m.group(6),
                    "stage": m.group(7),
                    "msg": m.group(8),
                }
            )
            continue

    out["fault_slices"] = [slices[idx] for idx in sorted(slices.keys())]
    return out


def pick_target_driver(parsed_snapshot: dict[str, Any], override: str | None) -> str:
    if override is not None and override != "":
        return override

    for fault in parsed_snapshot.get("fault_slices", []):
        if not isinstance(fault, dict):
            continue
        ai_failure = fault.get("ai_failure", {})
        if isinstance(ai_failure, dict):
            name = str(ai_failure.get("driver", ""))
            if name not in ("", "unknown", "none"):
                return name
        vec = fault.get("feature_vector", {})
        if isinstance(vec, dict):
            name = str(vec.get("driver", ""))
            if name not in ("", "unknown", "none"):
                return name
    return "uart"


def read_for(ser_obj: Any, seconds: float) -> str:
    end = time.time() + seconds
    out: list[str] = []
    while time.time() < end:
        n = ser_obj.in_waiting
        data = ser_obj.read(n or 1)
        if data:
            out.append(data.decode("utf-8", "replace"))
    return "".join(out)


def read_until_tokens(ser_obj: Any, timeout_s: float, tokens: tuple[str, ...]) -> str:
    end = time.time() + timeout_s
    out: list[str] = []
    joined = ""
    while time.time() < end:
        n = ser_obj.in_waiting
        data = ser_obj.read(n or 1)
        if not data:
            continue
        chunk = data.decode("utf-8", "replace")
        out.append(chunk)
        joined += chunk
        if tokens and all(token in joined for token in tokens):
            out.append(read_for(ser_obj, 0.30))
            return "".join(out)
    return "".join(out)


def sync_cli(ser_obj: Any) -> None:
    for _ in range(5):
        ser_obj.write(b"\r")
        time.sleep(0.03)
    _ = read_for(ser_obj, 0.35)


def send_line(ser_obj: Any, line: str, char_delay_s: float) -> None:
    for ch in (line + "\r"):
        ser_obj.write(ch.encode("utf-8"))
        if char_delay_s > 0.0:
            time.sleep(char_delay_s)


def run_command(
    ser_obj: Any,
    command: str,
    timeout_s: float,
    char_delay_s: float,
    done_tokens: tuple[str, ...] = (),
) -> str:
    sync_cli(ser_obj)
    send_line(ser_obj, command, char_delay_s)
    if done_tokens:
        return read_until_tokens(ser_obj, timeout_s, done_tokens)
    output = read_for(ser_obj, timeout_s)
    output += read_for(ser_obj, 0.20)
    return output


def extract_snapshot_from_log_text(text: str) -> str:
    lines = normalize_text(text).splitlines()
    begin_idx = -1
    end_idx = -1
    for i, line in enumerate(lines):
        if "snapshot begin" in line:
            begin_idx = i
            break
    if begin_idx >= 0:
        for i in range(begin_idx, len(lines)):
            if "snapshot end" in lines[i]:
                end_idx = i
                break
    if begin_idx >= 0 and end_idx >= begin_idx:
        return "\n".join(lines[begin_idx : end_idx + 1])
    filtered = [line for line in lines if line.strip().startswith("snapshot ")]
    return "\n".join(filtered)


def build_bundle_serial(
    port: str,
    baud: int,
    char_delay_ms: float,
    snapshot_timeout_s: float,
    target_driver_override: str | None,
) -> tuple[dict[str, Any], dict[str, str]]:
    if serial is None:
        raise RuntimeError("pyserial not installed. Run: pip3 install pyserial")

    char_delay_s = char_delay_ms / 1000.0
    raw: dict[str, str] = {
        "snapshot": "",
        "stage_wait_json": "",
        "stage_json": "",
        "dep_impact_json": "",
        "dep_whatif_reset_json": "",
        "dep_whatif_throttle_json": "",
        "dep_whatif_deny_json": "",
    }

    try:
        ser_obj = serial.Serial(port, baud, timeout=0.03)
    except Exception as exc:
        raise RuntimeError(f"open serial failed: {exc}") from exc

    try:
        _ = read_for(ser_obj, 1.0)
        sync_cli(ser_obj)
        send_line(ser_obj, "disable", char_delay_s)
        _ = read_for(ser_obj, 0.8)

        raw["snapshot"] = run_command(
            ser_obj,
            "snapshot",
            snapshot_timeout_s,
            char_delay_s,
            done_tokens=("snapshot end",),
        )
        parsed_snapshot = parse_snapshot_text(raw["snapshot"])
        target_driver = pick_target_driver(parsed_snapshot, target_driver_override)

        raw["stage_wait_json"] = run_command(
            ser_obj, "bringup stage wait-json", 1.5, char_delay_s
        )
        raw["stage_json"] = run_command(
            ser_obj, "bringup stage json", 1.8, char_delay_s
        )
        raw["dep_impact_json"] = run_command(
            ser_obj, f"dep impact-json {target_driver}", 1.8, char_delay_s
        )
        raw["dep_whatif_reset_json"] = run_command(
            ser_obj, f"dep whatif-json reset {target_driver}", 2.1, char_delay_s
        )
        raw["dep_whatif_throttle_json"] = run_command(
            ser_obj, f"dep whatif-json throttle {target_driver}", 2.1, char_delay_s
        )
        raw["dep_whatif_deny_json"] = run_command(
            ser_obj, f"dep whatif-json deny {target_driver}", 2.1, char_delay_s
        )
    finally:
        ser_obj.close()

    stage_wait_rows = parse_json_lines(raw["stage_wait_json"])
    stage_rows = parse_json_lines(raw["stage_json"])
    dep_impact_rows = parse_json_lines(raw["dep_impact_json"])

    dep_whatif_map = {
        "reset": dep_parse_whatif(parse_json_lines(raw["dep_whatif_reset_json"])),
        "throttle": dep_parse_whatif(parse_json_lines(raw["dep_whatif_throttle_json"])),
        "deny": dep_parse_whatif(parse_json_lines(raw["dep_whatif_deny_json"])),
    }

    bundle = {
        "ok": len(parsed_snapshot.get("fault_slices", [])) != 0,
        "source": {
            "mode": "serial",
            "port": port,
            "baud": baud,
        },
        "snapshot": parsed_snapshot,
        "stage": {
            "wait_json": stage_wait_rows,
            "stage_json": stage_rows,
        },
        "dependency": {
            "target_driver": pick_target_driver(parsed_snapshot, target_driver_override),
            "impact": dep_parse_impact(dep_impact_rows),
            "whatif": dep_whatif_map,
        },
    }
    return bundle, raw


def build_bundle_log(
    source_log: Path,
    target_driver_override: str | None,
) -> tuple[dict[str, Any], dict[str, str]]:
    text = source_log.read_text(encoding="utf-8")
    raw_snapshot = extract_snapshot_from_log_text(text)
    parsed_snapshot = parse_snapshot_text(raw_snapshot)
    target_driver = pick_target_driver(parsed_snapshot, target_driver_override)
    bundle = {
        "ok": len(parsed_snapshot.get("fault_slices", [])) != 0,
        "source": {
            "mode": "log",
            "path": str(source_log),
        },
        "snapshot": parsed_snapshot,
        "stage": {
            "wait_json": [],
            "stage_json": [],
        },
        "dependency": {
            "target_driver": target_driver,
            "impact": {"summary": {}, "drivers": []},
            "whatif": {
                "reset": {"summary": {}, "drivers": [], "stages": [], "secondary": []},
                "throttle": {"summary": {}, "drivers": [], "stages": [], "secondary": []},
                "deny": {"summary": {}, "drivers": [], "stages": [], "secondary": []},
            },
        },
    }
    return bundle, {"snapshot": raw_snapshot}


def write_bundle_artifacts(bundle_path: Path, raw: dict[str, str]) -> dict[str, str]:
    paths: dict[str, str] = {}
    stem = bundle_path.stem
    for key, text in raw.items():
        if text == "":
            continue
        out = bundle_path.parent / f"{stem}.{key}.log"
        out.write_text(normalize_text(text), encoding="utf-8")
        paths[key] = str(out)
    return paths


def write_replay_artifact(bundle_path: Path, bundle: dict[str, Any]) -> dict[str, Any]:
    replay = build_replay(bundle)
    replay_path = bundle_path.parent / f"{bundle_path.stem}.replay.json"
    replay_path.write_text(json.dumps(replay, ensure_ascii=True, indent=2) + "\n", encoding="utf-8")
    return {
        "path": str(replay_path),
        "schema": str(replay.get("schema", "")),
        "sha256": str(replay.get("replay_sha256", "")),
        "fault_slices": int(replay.get("summary", {}).get("fault_slice_count", 0)),
    }


def _run_seam_analyze(cfl_path: str) -> str:
    """Run 'mkdbg seam analyze <cfl_path>' and return stdout, or '' on failure."""
    try:
        result = subprocess.run(
            ["mkdbg", "seam", "analyze", cfl_path],
            capture_output=True,
            text=True,
        )
        if result.returncode == 0:
            return result.stdout
        print(
            f"[seam] causal analysis unavailable — mkdbg seam analyze returned {result.returncode}",
            file=sys.stderr,
        )
        return ""
    except FileNotFoundError:
        print(
            "[seam] causal analysis unavailable — install mkdbg and ensure it is on PATH",
            file=sys.stderr,
        )
        return ""


def main() -> int:
    ap = argparse.ArgumentParser(
        description=(
            "Build one triage bundle that contains snapshot/events/stage/dependency/feature/hypothesis."
        )
    )
    ap.add_argument("--port", help="serial port, e.g. /dev/cu.usbmodemXXXX")
    ap.add_argument("--baud", type=int, default=115200, help="UART baud rate")
    ap.add_argument("--char-delay-ms", type=float, default=10.0, help="per-char TX delay in ms")
    ap.add_argument(
        "--snapshot-timeout-s",
        type=float,
        default=60.0,
        help="timeout for snapshot capture in seconds",
    )
    ap.add_argument("--source-log", help="existing bringup/snapshot log path for offline bundle build")
    ap.add_argument("--target-driver", help="override dependency target driver")
    ap.add_argument("--output", help="output JSON path (default: logs/triage_bundle_<ts>.json)")
    ap.add_argument("--json", action="store_true", help="print machine-readable summary")
    ap.add_argument(
        "--seam-cfl",
        metavar="FILE",
        help="path to .cfl seam capture; appends CAUSAL CHAIN section to bundle output",
    )
    ns = ap.parse_args()

    if (ns.port is None and ns.source_log is None) or (ns.port is not None and ns.source_log is not None):
        print("error: specify exactly one of --port or --source-log", file=sys.stderr)
        return 2
    if ns.snapshot_timeout_s <= 0.0:
        print("error: --snapshot-timeout-s must be > 0", file=sys.stderr)
        return 2

    LOG_DIR.mkdir(parents=True, exist_ok=True)
    if ns.output is not None and ns.output != "":
        bundle_path = Path(ns.output)
    else:
        ts = time.strftime("%Y%m%d_%H%M%S")
        bundle_path = LOG_DIR / f"triage_bundle_{ts}.json"
    if not bundle_path.is_absolute():
        bundle_path = ROOT / bundle_path
    bundle_path.parent.mkdir(parents=True, exist_ok=True)

    try:
        if ns.port is not None:
            bundle, raw = build_bundle_serial(
                ns.port,
                ns.baud,
                ns.char_delay_ms,
                ns.snapshot_timeout_s,
                ns.target_driver,
            )
        else:
            source_log = Path(ns.source_log)
            if not source_log.is_absolute():
                source_log = ROOT / source_log
            bundle, raw = build_bundle_log(source_log, ns.target_driver)
    except Exception as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    bundle["time"] = time.strftime("%Y-%m-%d %H:%M:%S")
    bundle["bundle_path"] = str(bundle_path)
    bundle["artifact_logs"] = write_bundle_artifacts(bundle_path, raw)
    bundle["replay_artifact"] = write_replay_artifact(bundle_path, bundle)

    if ns.seam_cfl:
        bundle["seam_causal_chain"] = _run_seam_analyze(ns.seam_cfl)
    else:
        bundle["seam_causal_chain"] = ""

    bundle_path.write_text(json.dumps(bundle, ensure_ascii=True, indent=2) + "\n", encoding="utf-8")

    if ns.json:
        print(
            json.dumps(
                {
                    "ok": bundle["ok"],
                    "bundle_path": str(bundle_path),
                    "replay_path": bundle["replay_artifact"]["path"],
                    "replay_sha256": bundle["replay_artifact"]["sha256"],
                    "fault_slices": len(bundle["snapshot"].get("fault_slices", [])),
                    "target_driver": bundle["dependency"]["target_driver"],
                },
                ensure_ascii=True,
            )
        )
    else:
        print(f"bundle: {bundle_path}")
        print(f"replay: {bundle['replay_artifact']['path']}")
        print(f"replay_sha256: {bundle['replay_artifact']['sha256']}")
        print(f"fault_slices: {len(bundle['snapshot'].get('fault_slices', []))}")
        print(f"target_driver: {bundle['dependency']['target_driver']}")
        print(f"overall: {'PASS' if bundle['ok'] else 'FAIL'}")
        if bundle["seam_causal_chain"]:
            print()
            print("CAUSAL CHAIN")
            print("------------")
            print(bundle["seam_causal_chain"].rstrip())
    return 0 if bundle["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
