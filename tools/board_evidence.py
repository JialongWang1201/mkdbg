#!/usr/bin/env python3
"""Evaluate board-revision bring-up measurements from local instrument exports."""

import argparse
import csv
import json
import math
import sys
from pathlib import Path


def require(condition, message):
    if not condition:
        raise ValueError(message)


def load_object(path):
    with path.open(encoding="utf-8") as stream:
        result = json.load(stream)
    require(isinstance(result, dict), f"{path}: expected a JSON object")
    return result


def integer(value, name, minimum=0):
    require(type(value) is int and value >= minimum, f"{name}: expected integer >= {minimum}")
    return value


def finite(value, name):
    require(type(value) in (int, float) and math.isfinite(value), f"{name}: expected finite number")
    return value


def earlier(first, second):
    if first[1] < second[0]:
        return "before"
    if second[1] < first[0]:
        return "after"
    return "order-uncertain"


def evaluate(profile, paths, cpu=None):
    require(profile["version"] == 1, "unsupported board profile version")
    for key in ("board", "revision", "clock"):
        require(isinstance(profile[key], str) and profile[key], f"{key}: expected a name")
    probe = profile["probe"]
    for key in ("transport", "connector", "openocd_cfg"):
        require(isinstance(probe[key], str) and probe[key], f"probe.{key}: expected a value")
    signals = profile["signals"]
    require(isinstance(signals, dict) and signals, "signals: expected a nonempty object")
    trigger = profile["trigger"]
    require(trigger in signals, "trigger: unknown signal")
    instruments = profile["instruments"]
    edges = {}
    channels = set()
    for name, signal in signals.items():
        instrument, channel = signal["instrument"], signal["channel"]
        require(instrument in instruments, f"{name}: unknown instrument {instrument}")
        require((instrument, channel) not in channels, f"{name}: duplicate instrument channel")
        channels.add((instrument, channel))
        require(signal["kind"] in ("power", "reset", "boot-marker", "trigger"),
                f"{name}: unknown signal kind")
        require(signal["direction"] in ("rising", "falling"), f"{name}: invalid edge direction")
        finite(signal["threshold"], f"{name}.threshold")
        if name != trigger:
            lower = integer(signal["min_delay_ns"], f"{name}.min_delay_ns", -(2 ** 63))
            upper = integer(signal["max_delay_ns"], f"{name}.max_delay_ns", -(2 ** 63))
            require(lower <= upper, f"{name}: reversed delay bounds")
    require(set(paths) <= instruments.keys(), "sample export refers to an unknown instrument")

    for instrument, path in paths.items():
        timebase = instruments[instrument]
        offset = integer(timebase["offset_ns"], f"{instrument}.offset_ns", -(2 ** 63))
        error = integer(timebase["error_ns"], f"{instrument}.error_ns")
        mapping = {s["channel"]: (name, s) for name, s in signals.items()
                   if s["instrument"] == instrument}
        previous = {}
        with path.open(newline="", encoding="utf-8") as stream:
            rows = csv.DictReader(stream)
            require(rows.fieldnames == ["time_ns", "channel", "value"],
                    f"{path}: expected time_ns,channel,value CSV")
            for row in rows:
                channel = row["channel"]
                if channel not in mapping:
                    continue
                when = integer(int(row["time_ns"]), f"{path}:{rows.line_num} time_ns") + offset
                value = finite(float(row["value"]), f"{path}:{rows.line_num} value")
                name, signal = mapping[channel]
                active = (value >= signal["threshold"] if signal["direction"] == "rising"
                          else value <= signal["threshold"])
                if channel in previous:
                    last, was_active = previous[channel]
                    require(when > last, f"{path}:{rows.line_num}: non-increasing time on {channel}")
                    if active and not was_active:
                        edges.setdefault(name, []).append((last - error, when + error))
                previous[channel] = (when, active)

    trigger_edges = edges.get(trigger, [])
    require(len(trigger_edges) <= 1, "multiple trigger edges: select a single power-on capture")
    anchor = trigger_edges[0] if trigger_edges else None
    cpu_window = None
    if cpu is not None and cpu.get("clock") == profile["clock"]:
        start = integer(cpu["start_ns"], "CPU start_ns")
        end = integer(cpu["end_ns"], "CPU end_ns", start)
        cpu_window = (start, end)
    results = {}
    for name, signal in signals.items():
        found = edges.get(name, [])
        interval = found[0] if len(found) == 1 else None
        row = {"kind": signal["kind"], "interval_ns": interval, "edges": len(found)}
        if interval is None:
            row["status"] = "multiple-edges" if found else "missing"
        elif name == trigger:
            row["status"] = "observed"
        elif anchor is None:
            row["status"] = "trigger-missing"
        else:
            delay = (interval[0] - anchor[1], interval[1] - anchor[0])
            bounds = (signal["min_delay_ns"], signal["max_delay_ns"])
            row["delay_ns"] = delay
            if delay[0] >= bounds[0] and delay[1] <= bounds[1]:
                row["status"] = "within-window"
            elif delay[1] < bounds[0] or delay[0] > bounds[1]:
                row["status"] = "outside-window"
            else:
                row["status"] = "timing-uncertain"
        if interval is not None and cpu is not None:
            row["relative_to_cpu"] = earlier(interval, cpu_window) if cpu_window else "unaligned"
        results[name] = row
    return {"board": profile["board"], "revision": profile["revision"], "clock": profile["clock"],
            "probe": probe, "cpu": cpu, "signals": results}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("profile", type=Path)
    parser.add_argument("--samples", action="append", default=[], metavar="INSTRUMENT=CSV")
    parser.add_argument("--cpu-state", type=Path)
    args = parser.parse_args()
    try:
        exports = {}
        for export in args.samples:
            name, separator, path = export.partition("=")
            require(separator and name and path, "--samples requires INSTRUMENT=CSV")
            require(name not in exports, f"duplicate export for {name}")
            exports[name] = Path(path)
        report = evaluate(load_object(args.profile), exports,
                          load_object(args.cpu_state) if args.cpu_state else None)
        print(json.dumps(report, indent=2, allow_nan=False))
        return 0 if all(r["status"] in ("observed", "within-window")
                        for r in report["signals"].values()) else 1
    except (OSError, ValueError, OverflowError, KeyError, TypeError) as exc:
        parser.exit(2, f"board evidence: {exc}\n")


if __name__ == "__main__":
    raise SystemExit(main())
