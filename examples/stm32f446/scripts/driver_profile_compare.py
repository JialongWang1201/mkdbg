#!/usr/bin/env python3
import argparse
import json
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any


IRQ_BINS = ("0", "1_4", "5_15", "16_63", "64_255", "256p")
DMA_BINS = ("0", "1_24", "25_49", "50_74", "75_99", "100")
CAP_KEYS = ("mpu", "irq", "dma", "fault", "power", "reset")
STATE_KEYS = ("init", "probe", "ready", "active", "error", "reset", "dead")
UNSTABLE_STATES = ("error", "reset", "dead")


@dataclass
class DriverProfile:
    irq_dist: dict[str, int] = field(default_factory=lambda: {k: 0 for k in IRQ_BINS})
    dma_rx_dist: dict[str, int] = field(default_factory=lambda: {k: 0 for k in DMA_BINS})
    dma_tx_dist: dict[str, int] = field(default_factory=lambda: {k: 0 for k in DMA_BINS})
    cap_count: dict[str, int] = field(default_factory=lambda: {k: 0 for k in CAP_KEYS})
    cap_fail: dict[str, int] = field(default_factory=lambda: {k: 0 for k in CAP_KEYS})
    state_visit: dict[str, int] = field(default_factory=lambda: {k: 0 for k in STATE_KEYS})
    state_row: dict[str, dict[str, int]] = field(
        default_factory=lambda: {f: {t: 0 for t in STATE_KEYS} for f in STATE_KEYS}
    )
    irq_samples: int = 0
    dma_samples: int = 0
    kdi_rc: str = "unknown"


@dataclass
class ProfileSnapshot:
    meta: dict[str, Any] = field(default_factory=dict)
    drivers: dict[str, DriverProfile] = field(default_factory=dict)


@dataclass
class CompareConfig:
    dist_shift_threshold_pct: int = 20
    dist_new_bin_pct: int = 5
    jitter_min_delta: int = 2
    jitter_rate_delta_pct: int = 10
    loop_min_delta: int = 1


def _as_int(value: Any) -> int:
    if isinstance(value, bool):
        return int(value)
    if isinstance(value, int):
        return value
    if isinstance(value, float):
        return int(value)
    if isinstance(value, str):
        try:
            return int(value, 0)
        except Exception:
            return 0
    return 0


def _as_non_negative_int(value: Any) -> int:
    v = _as_int(value)
    return v if v > 0 else 0


def _driver_get(snapshot: ProfileSnapshot, driver: str) -> DriverProfile:
    if driver not in snapshot.drivers:
        snapshot.drivers[driver] = DriverProfile()
    return snapshot.drivers[driver]


def parse_profile_snapshots_text(text: str) -> list[ProfileSnapshot]:
    snapshots: list[ProfileSnapshot] = []
    current: ProfileSnapshot | None = None

    for raw in text.splitlines():
        line = raw.strip()
        if not line.startswith("{"):
            continue
        try:
            row = json.loads(line)
        except Exception:
            continue

        row_type = str(row.get("type", ""))
        if row_type == "driver-profile-meta":
            current = ProfileSnapshot(meta=row, drivers={})
            snapshots.append(current)
            continue

        if current is None:
            current = ProfileSnapshot(meta={"type": "driver-profile-meta", "version": 1}, drivers={})
            snapshots.append(current)

        driver = row.get("driver")
        if not isinstance(driver, str) or driver == "":
            continue
        profile = _driver_get(current, driver)

        if row_type == "driver-profile":
            profile.irq_samples = _as_non_negative_int(row.get("irq_samples", 0))
            profile.dma_samples = _as_non_negative_int(row.get("dma_samples", 0))
            profile.kdi_rc = str(row.get("kdi_rc", "unknown"))
            continue

        if row_type == "driver-profile-irq":
            dist = row.get("dist", {})
            if isinstance(dist, dict):
                for key in IRQ_BINS:
                    profile.irq_dist[key] = _as_non_negative_int(dist.get(key, 0))
            continue

        if row_type == "driver-profile-dma":
            rx = row.get("rx", {})
            tx = row.get("tx", {})
            if isinstance(rx, dict):
                for key in DMA_BINS:
                    profile.dma_rx_dist[key] = _as_non_negative_int(rx.get(key, 0))
            if isinstance(tx, dict):
                for key in DMA_BINS:
                    profile.dma_tx_dist[key] = _as_non_negative_int(tx.get(key, 0))
            continue

        if row_type == "driver-profile-kdi-a":
            for key in ("mpu", "irq", "dma"):
                item = row.get(key, {})
                if isinstance(item, dict):
                    profile.cap_count[key] = _as_non_negative_int(item.get("cnt", 0))
                    profile.cap_fail[key] = _as_non_negative_int(item.get("fail", 0))
            continue

        if row_type == "driver-profile-kdi-b":
            for key in ("fault", "power", "reset"):
                item = row.get(key, {})
                if isinstance(item, dict):
                    profile.cap_count[key] = _as_non_negative_int(item.get("cnt", 0))
                    profile.cap_fail[key] = _as_non_negative_int(item.get("fail", 0))
            continue

        if row_type == "driver-profile-state-visits":
            for key in STATE_KEYS:
                profile.state_visit[key] = _as_non_negative_int(row.get(key, 0))
            continue

        if row_type == "driver-profile-state-row":
            from_state = str(row.get("from", ""))
            if from_state in STATE_KEYS:
                for to_state in STATE_KEYS:
                    profile.state_row[from_state][to_state] = _as_non_negative_int(row.get(to_state, 0))
            continue

    return snapshots


def load_profile_snapshots(path: str) -> list[ProfileSnapshot]:
    data = Path(path).read_text(encoding="utf-8")
    return parse_profile_snapshots_text(data)


def select_snapshot(
    snapshots: list[ProfileSnapshot],
    index: int,
    label: str,
) -> tuple[ProfileSnapshot, int]:
    if len(snapshots) == 0:
        raise ValueError(f"{label}: no profile snapshots found")
    resolved = index
    if resolved < 0:
        resolved = len(snapshots) + resolved
    if resolved < 0 or resolved >= len(snapshots):
        raise ValueError(
            f"{label}: snapshot index {index} out of range (count={len(snapshots)})"
        )
    return snapshots[resolved], resolved


def _subtract_counts_map(
    keys: tuple[str, ...],
    later: dict[str, int],
    earlier: dict[str, int],
) -> tuple[dict[str, int], int]:
    out: dict[str, int] = {}
    underflow = 0
    for key in keys:
        lv = _as_non_negative_int(later.get(key, 0))
        ev = _as_non_negative_int(earlier.get(key, 0))
        if lv >= ev:
            out[key] = lv - ev
        else:
            out[key] = 0
            underflow += 1
    return out, underflow


def subtract_driver_profile(later: DriverProfile, earlier: DriverProfile) -> tuple[DriverProfile, int]:
    underflow = 0
    out = DriverProfile()

    out.irq_dist, u = _subtract_counts_map(IRQ_BINS, later.irq_dist, earlier.irq_dist)
    underflow += u
    out.dma_rx_dist, u = _subtract_counts_map(DMA_BINS, later.dma_rx_dist, earlier.dma_rx_dist)
    underflow += u
    out.dma_tx_dist, u = _subtract_counts_map(DMA_BINS, later.dma_tx_dist, earlier.dma_tx_dist)
    underflow += u
    out.cap_count, u = _subtract_counts_map(CAP_KEYS, later.cap_count, earlier.cap_count)
    underflow += u
    out.cap_fail, u = _subtract_counts_map(CAP_KEYS, later.cap_fail, earlier.cap_fail)
    underflow += u
    out.state_visit, u = _subtract_counts_map(STATE_KEYS, later.state_visit, earlier.state_visit)
    underflow += u

    out.state_row = {}
    for from_state in STATE_KEYS:
        out.state_row[from_state], u = _subtract_counts_map(
            STATE_KEYS, later.state_row[from_state], earlier.state_row[from_state]
        )
        underflow += u

    if later.irq_samples >= earlier.irq_samples:
        out.irq_samples = later.irq_samples - earlier.irq_samples
    else:
        out.irq_samples = 0
        underflow += 1

    if later.dma_samples >= earlier.dma_samples:
        out.dma_samples = later.dma_samples - earlier.dma_samples
    else:
        out.dma_samples = 0
        underflow += 1

    out.kdi_rc = later.kdi_rc
    return out, underflow


def build_period_delta_snapshot(
    baseline: ProfileSnapshot,
    candidate: ProfileSnapshot,
) -> tuple[ProfileSnapshot, int]:
    delta = ProfileSnapshot(meta=dict(candidate.meta), drivers={})
    underflow_total = 0

    for driver, later_profile in candidate.drivers.items():
        earlier_profile = baseline.drivers.get(driver)
        if earlier_profile is None:
            delta.drivers[driver] = later_profile
            continue
        diff, underflow = subtract_driver_profile(later_profile, earlier_profile)
        delta.drivers[driver] = diff
        underflow_total += underflow

    return delta, underflow_total


def _dist_prob(keys: tuple[str, ...], counts: dict[str, int]) -> tuple[list[float], int]:
    vals = [_as_non_negative_int(counts.get(k, 0)) for k in keys]
    total = sum(vals)
    if total == 0:
        return [0.0 for _ in keys], 0
    return [v / float(total) for v in vals], total


def _dist_tvd_pct(keys: tuple[str, ...], base: dict[str, int], cand: dict[str, int]) -> tuple[int, int, int]:
    bp, bt = _dist_prob(keys, base)
    cp, ct = _dist_prob(keys, cand)
    if bt == 0 and ct == 0:
        return 0, bt, ct
    if bt == 0 or ct == 0:
        return 100, bt, ct
    tvd = 0.0
    for i in range(len(keys)):
        tvd += abs(bp[i] - cp[i])
    tvd *= 0.5
    return int(round(tvd * 100.0)), bt, ct


def _dist_new_bin_mass_pct(
    keys: tuple[str, ...],
    base: dict[str, int],
    cand: dict[str, int],
) -> int:
    cp, ct = _dist_prob(keys, cand)
    if ct == 0:
        return 0
    mass = 0.0
    for i, key in enumerate(keys):
        if _as_non_negative_int(base.get(key, 0)) == 0 and _as_non_negative_int(cand.get(key, 0)) > 0:
            mass += cp[i]
    return int(round(mass * 100.0))


def _state_total_transitions(profile: DriverProfile) -> int:
    total = 0
    for from_state in STATE_KEYS:
        for to_state in STATE_KEYS:
            if from_state == to_state:
                continue
            total += _as_non_negative_int(profile.state_row[from_state].get(to_state, 0))
    return total


def _state_jitter_transitions(profile: DriverProfile) -> int:
    total = 0
    for from_state in STATE_KEYS:
        for to_state in STATE_KEYS:
            if from_state == to_state:
                continue
            if from_state in UNSTABLE_STATES or to_state in UNSTABLE_STATES:
                total += _as_non_negative_int(profile.state_row[from_state].get(to_state, 0))
    return total


def _state_loop_count(profile: DriverProfile) -> int:
    er = _as_non_negative_int(profile.state_row["error"].get("reset", 0))
    ri = _as_non_negative_int(profile.state_row["reset"].get("init", 0))
    ae = _as_non_negative_int(profile.state_row["active"].get("error", 0))
    pe = _as_non_negative_int(profile.state_row["probe"].get("error", 0))
    return min(er, ri) + min(ae, er) + min(pe, er)


def _pct(part: int, total: int) -> int:
    if total <= 0:
        return 0
    return int(round((float(part) * 100.0) / float(total)))


def _anomaly_distribution(
    metric: str,
    keys: tuple[str, ...],
    base_counts: dict[str, int],
    cand_counts: dict[str, int],
    cfg: CompareConfig,
) -> dict[str, Any] | None:
    tvd_pct, base_total, cand_total = _dist_tvd_pct(keys, base_counts, cand_counts)
    new_bin_mass_pct = _dist_new_bin_mass_pct(keys, base_counts, cand_counts)
    shifted = (
        tvd_pct >= cfg.dist_shift_threshold_pct
        or new_bin_mass_pct >= cfg.dist_new_bin_pct
    )
    if not shifted:
        return None
    return {
        "type": "distribution_shift",
        "metric": metric,
        "tvd_pct": tvd_pct,
        "new_bin_mass_pct": new_bin_mass_pct,
        "baseline_total": base_total,
        "candidate_total": cand_total,
    }


def compare_driver_profiles(
    driver: str,
    baseline: DriverProfile,
    candidate: DriverProfile,
    cfg: CompareConfig,
    baseline_label: str,
    candidate_label: str,
) -> dict[str, Any]:
    anomalies: list[dict[str, Any]] = []

    for metric, keys, b_counts, c_counts in (
        ("irq_rate", IRQ_BINS, baseline.irq_dist, candidate.irq_dist),
        ("dma_rx_occupancy", DMA_BINS, baseline.dma_rx_dist, candidate.dma_rx_dist),
        ("dma_tx_occupancy", DMA_BINS, baseline.dma_tx_dist, candidate.dma_tx_dist),
    ):
        anomaly = _anomaly_distribution(metric, keys, b_counts, c_counts, cfg)
        if anomaly is not None:
            anomalies.append(anomaly)

    new_caps: list[dict[str, int | str]] = []
    for cap in CAP_KEYS:
        b_cnt = _as_non_negative_int(baseline.cap_count.get(cap, 0))
        c_cnt = _as_non_negative_int(candidate.cap_count.get(cap, 0))
        if b_cnt == 0 and c_cnt > 0:
            new_caps.append(
                {
                    "capability": cap,
                    "candidate_count": c_cnt,
                    "candidate_fail": _as_non_negative_int(candidate.cap_fail.get(cap, 0)),
                }
            )
    if len(new_caps) != 0:
        anomalies.append(
            {
                "type": "new_capability_usage",
                "new_capabilities": new_caps,
            }
        )

    base_total = _state_total_transitions(baseline)
    cand_total = _state_total_transitions(candidate)
    base_jitter = _state_jitter_transitions(baseline)
    cand_jitter = _state_jitter_transitions(candidate)
    base_loop = _state_loop_count(baseline)
    cand_loop = _state_loop_count(candidate)
    jitter_delta = cand_jitter - base_jitter
    loop_delta = cand_loop - base_loop
    base_rate = _pct(base_jitter, base_total)
    cand_rate = _pct(cand_jitter, cand_total)
    jitter_rate_delta = cand_rate - base_rate

    if (
        jitter_delta >= cfg.jitter_min_delta
        or loop_delta >= cfg.loop_min_delta
        or jitter_rate_delta >= cfg.jitter_rate_delta_pct
    ):
        anomalies.append(
            {
                "type": "state_jitter_increase",
                "baseline_jitter": base_jitter,
                "candidate_jitter": cand_jitter,
                "jitter_delta": jitter_delta,
                "baseline_jitter_rate_pct": base_rate,
                "candidate_jitter_rate_pct": cand_rate,
                "jitter_rate_delta_pct": jitter_rate_delta,
                "baseline_loops": base_loop,
                "candidate_loops": cand_loop,
                "loop_delta": loop_delta,
            }
        )

    score = 0
    for anomaly in anomalies:
        if anomaly["type"] == "distribution_shift":
            score += max(10, min(40, _as_non_negative_int(anomaly.get("tvd_pct", 0))))
            score += min(15, _as_non_negative_int(anomaly.get("new_bin_mass_pct", 0)) // 2)
        elif anomaly["type"] == "new_capability_usage":
            score += 20 + 8 * len(anomaly.get("new_capabilities", []))
        elif anomaly["type"] == "state_jitter_increase":
            score += 25 + min(20, 3 * max(0, _as_int(anomaly.get("jitter_delta", 0))))

    regression = len(anomalies) != 0
    confidence = 5 if not regression else min(99, max(35, score))

    explain_parts: list[str] = []
    explain_parts.append(
        f"Compared {baseline_label} to {candidate_label} for driver {driver}, "
        f"{'a regression signature is present' if regression else 'no regression signature is present'}."
    )

    dist_items = [a for a in anomalies if a["type"] == "distribution_shift"]
    if len(dist_items) != 0:
        segs = []
        for item in dist_items:
            segs.append(
                f"{item['metric']} tvd={item['tvd_pct']}% (new-bin-mass={item['new_bin_mass_pct']}%)"
            )
        explain_parts.append(
            "Distribution shifted across behavior bins: " + ", ".join(segs) + "."
        )

    cap_items = [a for a in anomalies if a["type"] == "new_capability_usage"]
    if len(cap_items) != 0:
        cap_names: list[str] = []
        for entry in cap_items[0]["new_capabilities"]:
            cap_names.append(
                f"{entry['capability']}={entry['candidate_count']} (fail={entry['candidate_fail']})"
            )
        explain_parts.append(
            "New capability usage appeared in candidate period: " + ", ".join(cap_names) + "."
        )

    jitter_items = [a for a in anomalies if a["type"] == "state_jitter_increase"]
    if len(jitter_items) != 0:
        j = jitter_items[0]
        explain_parts.append(
            "State jitter increased with "
            f"jitter {j['baseline_jitter']}->{j['candidate_jitter']} (delta={j['jitter_delta']}), "
            f"jitter-rate {j['baseline_jitter_rate_pct']}%->{j['candidate_jitter_rate_pct']}%, "
            f"loop-count {j['baseline_loops']}->{j['candidate_loops']}."
        )

    if regression:
        explain_parts.append(
            "The candidate behavior departs from baseline in ways consistent with an operational regression, "
            "not just sampling noise."
        )
    else:
        explain_parts.append(
            "Core distributions, capability surface, and state-stability signals remain within baseline envelope."
        )

    return {
        "driver": driver,
        "regression": regression,
        "confidence": confidence,
        "anomalies": anomalies,
        "regression_explain": " ".join(explain_parts),
    }


def _print_human(report: dict[str, Any]) -> None:
    print(
        f"baseline={report['baseline']['path']} snapshot={report['baseline']['snapshot_index']} "
        f"({report['baseline']['label']})"
    )
    print(
        f"candidate={report['candidate']['path']} snapshot={report['candidate']['snapshot_index']} "
        f"({report['candidate']['label']})"
    )
    print(
        f"period_compare={report['period_compare']} underflow_fields={report['underflow_fields']} "
        f"drivers={report['driver_count']}"
    )
    for result in report["results"]:
        status = "REGRESSION" if result["regression"] else "OK"
        print(
            f"[{status}] driver={result['driver']} confidence={result['confidence']} "
            f"anomaly_count={len(result['anomalies'])}"
        )
        if len(result["anomalies"]) != 0:
            tags = []
            for anomaly in result["anomalies"]:
                if anomaly["type"] == "distribution_shift":
                    tags.append(f"distribution_shift:{anomaly['metric']}")
                elif anomaly["type"] == "new_capability_usage":
                    tags.append("new_capability_usage")
                else:
                    tags.append(anomaly["type"])
            print("  anomalies=" + ",".join(tags))
        print("  explain=" + result["regression_explain"])
    print(
        f"overall={'REGRESSION' if report['regression_count'] != 0 else 'OK'} "
        f"regression_count={report['regression_count']}"
    )


def _read_snapshots(path: str, index: int, label: str) -> tuple[list[ProfileSnapshot], ProfileSnapshot, int]:
    snapshots = load_profile_snapshots(path)
    selected, resolved_index = select_snapshot(snapshots, index, label)
    return snapshots, selected, resolved_index


def main() -> int:
    ap = argparse.ArgumentParser(
        description=(
            "Compare Driver Profile snapshots for regression signals "
            "(distribution shift, new capability usage, state jitter increase)."
        )
    )
    ap.add_argument("--baseline", required=True, help="baseline profile log/jsonl file")
    ap.add_argument("--candidate", required=True, help="candidate profile log/jsonl file")
    ap.add_argument("--baseline-index", type=int, default=-1, help="baseline snapshot index (default: latest)")
    ap.add_argument("--candidate-index", type=int, default=-1, help="candidate snapshot index (default: latest)")
    ap.add_argument("--driver", action="append", default=[], help="driver name (repeatable), default=all shared drivers")
    ap.add_argument("--period-compare", action="store_true", help="treat candidate as cumulative and compare baseline period vs candidate-baseline delta")
    ap.add_argument("--dist-shift-threshold-pct", type=int, default=20, help="distribution shift TVD threshold percent")
    ap.add_argument("--dist-new-bin-pct", type=int, default=5, help="new-bin probability mass threshold percent")
    ap.add_argument("--jitter-min-delta", type=int, default=2, help="minimum jitter-transition increase to flag")
    ap.add_argument("--jitter-rate-delta-pct", type=int, default=10, help="minimum jitter-rate increase percent to flag")
    ap.add_argument("--loop-min-delta", type=int, default=1, help="minimum state-loop increase to flag")
    ap.add_argument("--baseline-label", default="baseline", help="baseline label for explanation")
    ap.add_argument("--candidate-label", default="candidate", help="candidate label for explanation")
    ap.add_argument("--fail-on-regression", action="store_true", help="return exit code 1 if any driver regresses")
    ap.add_argument("--json", action="store_true", help="emit JSON report")
    ns = ap.parse_args()

    try:
        _, baseline_snapshot, baseline_index = _read_snapshots(ns.baseline, ns.baseline_index, "baseline")
        _, candidate_snapshot_raw, candidate_index = _read_snapshots(ns.candidate, ns.candidate_index, "candidate")
    except Exception as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    candidate_snapshot = candidate_snapshot_raw
    underflow_fields = 0
    if ns.period_compare:
        candidate_snapshot, underflow_fields = build_period_delta_snapshot(
            baseline_snapshot, candidate_snapshot_raw
        )
        if underflow_fields != 0:
            print(
                "error: period compare requested but candidate snapshot is not monotonic "
                f"(underflow_fields={underflow_fields})",
                file=sys.stderr,
            )
            return 2

    if len(ns.driver) != 0:
        drivers = sorted(set(ns.driver))
    else:
        drivers = sorted(
            set(baseline_snapshot.drivers.keys()).intersection(
                set(candidate_snapshot.drivers.keys())
            )
        )

    if len(drivers) == 0:
        print("error: no comparable drivers found", file=sys.stderr)
        return 2

    cfg = CompareConfig(
        dist_shift_threshold_pct=max(0, ns.dist_shift_threshold_pct),
        dist_new_bin_pct=max(0, ns.dist_new_bin_pct),
        jitter_min_delta=max(0, ns.jitter_min_delta),
        jitter_rate_delta_pct=max(0, ns.jitter_rate_delta_pct),
        loop_min_delta=max(0, ns.loop_min_delta),
    )

    results: list[dict[str, Any]] = []
    missing: list[str] = []
    for driver in drivers:
        base_profile = baseline_snapshot.drivers.get(driver)
        cand_profile = candidate_snapshot.drivers.get(driver)
        if base_profile is None or cand_profile is None:
            missing.append(driver)
            continue
        result = compare_driver_profiles(
            driver,
            base_profile,
            cand_profile,
            cfg,
            ns.baseline_label,
            ns.candidate_label,
        )
        results.append(result)

    if len(missing) != 0:
        print(
            "error: driver missing from selected snapshots: " + ", ".join(sorted(missing)),
            file=sys.stderr,
        )
        return 2

    regression_count = sum(1 for r in results if r["regression"])
    report = {
        "ok": True,
        "period_compare": bool(ns.period_compare),
        "underflow_fields": underflow_fields,
        "driver_count": len(results),
        "regression_count": regression_count,
        "baseline": {
            "path": str(Path(ns.baseline)),
            "snapshot_index": baseline_index,
            "label": ns.baseline_label,
        },
        "candidate": {
            "path": str(Path(ns.candidate)),
            "snapshot_index": candidate_index,
            "label": ns.candidate_label,
        },
        "results": results,
    }

    if ns.json:
        print(json.dumps(report, ensure_ascii=True))
    else:
        _print_human(report)

    if ns.fail_on_regression and regression_count != 0:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
