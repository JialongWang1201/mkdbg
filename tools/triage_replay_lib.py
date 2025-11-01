#!/usr/bin/env python3
import copy
import hashlib
import json
from typing import Any


def _path_join(base: str, elem: str) -> str:
    if base == "":
        return elem
    if elem.startswith("["):
        return f"{base}{elem}"
    return f"{base}.{elem}"


def _stable_flags(raw: Any) -> list[str]:
    if not isinstance(raw, str):
        return []
    parts = [item.strip() for item in raw.split("|")]
    return sorted([item for item in parts if item not in ("", "none")])


def _stable_text(raw: Any) -> str:
    return " ".join(str(raw).split())


def _event_signature(event: dict[str, Any]) -> str:
    stage = str(event.get("stage", "none"))
    flags = _stable_flags(event.get("flags", "none"))
    msg = _stable_text(event.get("msg", ""))
    flag_text = ",".join(flags) if flags else "none"
    return f"stage={stage}|flags={flag_text}|msg={msg}"


def _first_row(rows: Any, row_type: str) -> dict[str, Any]:
    if not isinstance(rows, list):
        return {}
    for row in rows:
        if isinstance(row, dict) and str(row.get("type", "")) == row_type:
            return copy.deepcopy(row)
    return {}


def _canonical_hypotheses(items: Any) -> list[dict[str, Any]]:
    out: list[dict[str, Any]] = []
    if not isinstance(items, list):
        return out
    for item in items:
        if not isinstance(item, dict):
            continue
        out.append(
            {
                "name": str(item.get("name", "")),
                "confidence": int(item.get("confidence", 0)),
                "evidence_ids": list(item.get("evidence_ids", [])),
                "explain_p1": str(item.get("explain_p1", "")),
                "explain_p2": str(item.get("explain_p2", "")),
            }
        )
    out.sort(key=lambda item: (item["name"], -item["confidence"]))
    return out


def _canonical_support(items: Any) -> list[dict[str, Any]]:
    out: list[dict[str, Any]] = []
    if not isinstance(items, list):
        return out
    for item in items:
        if not isinstance(item, dict):
            continue
        msg = str(item.get("msg", ""))
        out.append(
            {
                "event_id": int(item.get("event_id", 0)),
                "stage": str(item.get("stage", "none")),
                "msg": msg,
                "signature": f"stage={item.get('stage', 'none')}|msg={_stable_text(msg)}",
            }
        )
    out.sort(key=lambda item: (item["event_id"], item["signature"]))
    return out


def _canonical_checkpoints(items: Any) -> list[dict[str, Any]]:
    out: list[dict[str, Any]] = []
    if not isinstance(items, list):
        return out
    for item in items:
        if not isinstance(item, dict):
            continue
        out.append(
            {
                "step": int(item.get("step", 0)),
                "cmd": str(item.get("cmd", "")),
                "focus": str(item.get("focus", "")),
            }
        )
    out.sort(key=lambda item: (item["step"], item["cmd"], item["focus"]))
    return out


def _canonical_events(items: Any) -> list[dict[str, Any]]:
    out: list[dict[str, Any]] = []
    if not isinstance(items, list):
        return out
    for idx, item in enumerate(items):
        if not isinstance(item, dict):
            continue
        out.append(
            {
                "ordinal": idx,
                "event_id": int(item.get("event_id", idx)),
                "stage": str(item.get("stage", "none")),
                "flags": _stable_flags(item.get("flags", "none")),
                "signature": _event_signature(item),
            }
        )
    return out


def _canonical_slice(slice_obj: dict[str, Any]) -> dict[str, Any]:
    meta = slice_obj.get("meta", {}) if isinstance(slice_obj.get("meta"), dict) else {}
    failure = slice_obj.get("failure", {}) if isinstance(slice_obj.get("failure"), dict) else {}
    ai_failure = (
        slice_obj.get("ai_failure", {}) if isinstance(slice_obj.get("ai_failure"), dict) else {}
    )
    feature_vector = (
        slice_obj.get("feature_vector", {})
        if isinstance(slice_obj.get("feature_vector"), dict)
        else {}
    )
    feature_detail = (
        slice_obj.get("feature_detail", {})
        if isinstance(slice_obj.get("feature_detail"), dict)
        else {}
    )
    driver = str(ai_failure.get("driver", "")) or str(feature_vector.get("driver", "")) or "unknown"
    events = _canonical_events(slice_obj.get("events", []))
    failure_explain = (
        slice_obj.get("failure_explain", {})
        if isinstance(slice_obj.get("failure_explain"), dict)
        else {}
    )
    return {
        "slice_id": int(slice_obj.get("slice_id", 0)),
        "corr_id": str(meta.get("corr_id", "")),
        "stage": str(meta.get("stage", "none")),
        "driver": driver,
        "events_total": int(meta.get("events", len(events))),
        "fault_events": int(meta.get("fault_events", 0)),
        "reset_events": int(meta.get("reset_events", 0)),
        "failure": {
            "category": str(failure.get("category", "none")),
            "confidence": int(failure.get("confidence", 0)),
            "stage": str(failure.get("stage", "none")),
            "evidence_ids": list(failure.get("evidence_ids", [])),
            "explain_p1": str(failure_explain.get("p1", "")),
            "explain_p2": str(failure_explain.get("p2", "")),
        },
        "ai_failure": {
            "class": str(ai_failure.get("class", "none")),
            "stage": str(ai_failure.get("stage", "none")),
            "driver": str(ai_failure.get("driver", "unknown")),
            "evidence_ids": list(ai_failure.get("evidence_ids", [])),
        },
        "feature_vector": {
            "driver": str(feature_vector.get("driver", driver)),
            "irq_abnormal": int(feature_vector.get("irq_abnormal", 0)),
            "dma_full": int(feature_vector.get("dma_full", 0)),
            "dma_idle_spin": int(feature_vector.get("dma_idle_spin", 0)),
            "kdi_fail_frequent": int(feature_vector.get("kdi_fail_frequent", 0)),
            "state_error_reset_loop": int(feature_vector.get("state_error_reset_loop", 0)),
            "cap_eperm": int(feature_vector.get("cap_eperm", 0)),
            "cap_mpu_violation": int(feature_vector.get("cap_mpu_violation", 0)),
        },
        "feature_detail": {
            "irq_event_count": int(feature_detail.get("irq_event_count", 0)),
            "irq_rate_per_sec": int(feature_detail.get("irq_rate_per_sec", 0)),
            "kdi_fail_count": int(feature_detail.get("kdi_fail_count", 0)),
            "lookback_ms": int(feature_detail.get("lookback_ms", 0)),
        },
        "hypotheses": _canonical_hypotheses(slice_obj.get("hypotheses", [])),
        "ai_support": _canonical_support(slice_obj.get("ai_support", [])),
        "ai_checkpoints": _canonical_checkpoints(slice_obj.get("ai_checkpoints", [])),
        "events": events,
    }


def build_replay(bundle: dict[str, Any]) -> dict[str, Any]:
    snapshot = bundle.get("snapshot", {}) if isinstance(bundle.get("snapshot"), dict) else {}
    bringup = snapshot.get("bringup", {}) if isinstance(snapshot.get("bringup"), dict) else {}
    dependency = bundle.get("dependency", {}) if isinstance(bundle.get("dependency"), dict) else {}
    impact = dependency.get("impact", {}) if isinstance(dependency.get("impact"), dict) else {}
    whatif = dependency.get("whatif", {}) if isinstance(dependency.get("whatif"), dict) else {}
    slices = [_canonical_slice(item) for item in snapshot.get("fault_slices", []) if isinstance(item, dict)]

    replay = {
        "schema": "triage-replay/v1",
        "source": copy.deepcopy(bundle.get("source", {})),
        "summary": {
            "fault_slice_count": len(slices),
            "target_driver": str(dependency.get("target_driver", "unknown")),
            "boot_complete": int(bringup.get("boot_complete", 0)),
            "active": str(bringup.get("active", "none")),
            "last_error": int(bringup.get("last_error", 0)),
            "slice_drivers": [item["driver"] for item in slices],
            "event_signature_count": sum(len(item["events"]) for item in slices),
        },
        "stage": {
            "wait_summary": _first_row(bundle.get("stage", {}).get("wait_json", []), "bringup-stage-wait-summary"),
            "stage_summary": _first_row(bundle.get("stage", {}).get("stage_json", []), "bringup-stage-summary"),
        },
        "dependency": {
            "target_driver": str(dependency.get("target_driver", "unknown")),
            "impact_summary": copy.deepcopy(impact.get("summary", {})),
            "whatif_summaries": {
                "reset": copy.deepcopy(whatif.get("reset", {}).get("summary", {})),
                "throttle": copy.deepcopy(whatif.get("throttle", {}).get("summary", {})),
                "deny": copy.deepcopy(whatif.get("deny", {}).get("summary", {})),
            },
        },
        "fault_slices": slices,
    }

    digest_obj = {
        "summary": replay["summary"],
        "stage": replay["stage"],
        "dependency": replay["dependency"],
        "fault_slices": replay["fault_slices"],
    }
    digest_text = json.dumps(digest_obj, ensure_ascii=True, sort_keys=True, separators=(",", ":"))
    replay["replay_sha256"] = hashlib.sha256(digest_text.encode("utf-8")).hexdigest()
    return replay


def diff_json(baseline: Any, candidate: Any, path: str, out: list[dict[str, Any]]) -> None:
    if type(baseline) is not type(candidate):
        out.append(
            {
                "path": path or "$",
                "kind": "type",
                "baseline": type(baseline).__name__,
                "candidate": type(candidate).__name__,
            }
        )
        return

    if isinstance(baseline, dict):
        keys = sorted(set(baseline.keys()) | set(candidate.keys()))
        for key in keys:
            next_path = _path_join(path, key)
            if key not in baseline:
                out.append({"path": next_path, "kind": "added", "candidate": candidate[key]})
            elif key not in candidate:
                out.append({"path": next_path, "kind": "removed", "baseline": baseline[key]})
            else:
                diff_json(baseline[key], candidate[key], next_path, out)
        return

    if isinstance(baseline, list):
        common = min(len(baseline), len(candidate))
        for idx in range(common):
            diff_json(baseline[idx], candidate[idx], _path_join(path, f"[{idx}]"), out)
        for idx in range(common, len(baseline)):
            out.append(
                {
                    "path": _path_join(path, f"[{idx}]"),
                    "kind": "removed",
                    "baseline": baseline[idx],
                }
            )
        for idx in range(common, len(candidate)):
            out.append(
                {
                    "path": _path_join(path, f"[{idx}]"),
                    "kind": "added",
                    "candidate": candidate[idx],
                }
            )
        return

    if baseline != candidate:
        out.append(
            {
                "path": path or "$",
                "kind": "value",
                "baseline": baseline,
                "candidate": candidate,
            }
        )


def build_diff(baseline: dict[str, Any], candidate: dict[str, Any]) -> dict[str, Any]:
    changes: list[dict[str, Any]] = []
    diff_json(baseline, candidate, "", changes)
    return {
        "schema": "triage-replay-diff/v1",
        "same": len(changes) == 0,
        "change_count": len(changes),
        "baseline_sha256": str(baseline.get("replay_sha256", "")),
        "candidate_sha256": str(candidate.get("replay_sha256", "")),
        "changes": changes,
    }
