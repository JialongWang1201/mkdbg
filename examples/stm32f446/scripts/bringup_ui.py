#!/usr/bin/env python3
import argparse
import curses
import json
import textwrap
import time
from pathlib import Path
from typing import Any

import triage_bundle


DEFAULT_WIDTH = 120
DEFAULT_HEIGHT = 38
DEFAULT_BAUD = 115200
DEFAULT_CHAR_DELAY_MS = 0.0
DEFAULT_SNAPSHOT_TIMEOUT_S = 6.0
DEFAULT_AUTO_REFRESH_S = 0.0
MIN_WIDTH = 84
MIN_HEIGHT = 26
AUTO_REFRESH_OPTIONS = [0.0, 2.0, 5.0, 10.0, 15.0, 30.0]


def parse_args() -> argparse.Namespace:
    ap = argparse.ArgumentParser(
        description=(
            "Bringup/fault dashboard for MicroKernel-MPU. "
            "It can render one bundle offline or open an interactive terminal UI."
        )
    )
    ap.add_argument("--port", help="serial port for live bundle capture")
    ap.add_argument("--bundle-json", help="existing triage bundle json path")
    ap.add_argument("--source-log", help="existing snapshot/bringup log path")
    ap.add_argument("--baud", type=int, default=DEFAULT_BAUD, help="UART baud rate")
    ap.add_argument(
        "--char-delay-ms",
        type=float,
        default=DEFAULT_CHAR_DELAY_MS,
        help="per-char TX delay in ms for live capture",
    )
    ap.add_argument(
        "--snapshot-timeout-s",
        type=float,
        default=DEFAULT_SNAPSHOT_TIMEOUT_S,
        help="snapshot timeout for live capture",
    )
    ap.add_argument(
        "--auto-refresh-s",
        type=float,
        default=DEFAULT_AUTO_REFRESH_S,
        help="initial auto refresh interval in seconds (0 disables)",
    )
    ap.add_argument("--target-driver", help="override dependency target driver")
    ap.add_argument(
        "--render-once",
        action="store_true",
        help="print one dashboard frame to stdout instead of opening curses",
    )
    ap.add_argument("--width", type=int, default=DEFAULT_WIDTH, help="render width for --render-once")
    ap.add_argument("--height", type=int, default=DEFAULT_HEIGHT, help="render height for --render-once")
    ns = ap.parse_args()

    modes = [ns.port is not None, ns.bundle_json is not None, ns.source_log is not None]
    if sum(1 for enabled in modes if enabled) != 1:
        ap.error("choose exactly one of --port, --bundle-json, or --source-log")
    if ns.snapshot_timeout_s <= 0.0:
        ap.error("--snapshot-timeout-s must be > 0")
    if ns.auto_refresh_s < 0.0:
        ap.error("--auto-refresh-s must be >= 0")
    if ns.width < MIN_WIDTH:
        ap.error(f"--width must be >= {MIN_WIDTH}")
    if ns.height < MIN_HEIGHT:
        ap.error(f"--height must be >= {MIN_HEIGHT}")
    return ns


def load_bundle_json(path: Path) -> dict[str, Any]:
    obj = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(obj, dict):
        raise ValueError(f"bundle must be a JSON object: {path}")
    return obj


def normalize_driver_name(name: str) -> str:
    return name.replace("_", "-")


def get_rows_by_type(rows: list[dict[str, Any]], row_type: str) -> list[dict[str, Any]]:
    return [row for row in rows if str(row.get("type", "")) == row_type]


def get_first_row(rows: list[dict[str, Any]], row_type: str) -> dict[str, Any]:
    matches = get_rows_by_type(rows, row_type)
    return matches[0] if matches else {}


def evidence_text(ids: list[int]) -> str:
    return ",".join(str(item) for item in ids) if ids else "none"


def shorten(text: str, limit: int) -> str:
    if len(text) <= limit:
        return text
    if limit <= 3:
        return text[:limit]
    return text[: limit - 3] + "..."


def wrap_lines(lines: list[str], width: int) -> list[str]:
    out: list[str] = []
    inner_width = max(1, width)
    for line in lines:
        if line == "":
            out.append("")
            continue
        wrapped = textwrap.wrap(
            line,
            width=inner_width,
            replace_whitespace=False,
            drop_whitespace=False,
            break_long_words=True,
            break_on_hyphens=False,
        )
        out.extend(wrapped if wrapped else [""])
    return out


def fmt_seconds(value: float) -> str:
    if value <= 0.0:
        return "off"
    if abs(value - round(value)) < 0.001:
        return f"{int(round(value))}s"
    return f"{value:.1f}s"


def nearest_auto_refresh(value: float) -> float:
    return min(AUTO_REFRESH_OPTIONS, key=lambda item: abs(item - value))


def next_auto_refresh(value: float, delta: int) -> float:
    current = nearest_auto_refresh(value)
    idx = AUTO_REFRESH_OPTIONS.index(current)
    idx = max(0, min(len(AUTO_REFRESH_OPTIONS) - 1, idx + delta))
    return AUTO_REFRESH_OPTIONS[idx]


def render_panel(title: str, lines: list[str], width: int, height: int) -> list[str]:
    inner_width = max(1, width - 2)
    title_text = f" {title} "
    if len(title_text) > inner_width:
        title_text = title_text[:inner_width]
    left = max(0, (inner_width - len(title_text)) // 2)
    right = max(0, inner_width - len(title_text) - left)
    top = "+" + ("-" * left) + title_text + ("-" * right) + "+"
    body = wrap_lines(lines, inner_width)
    body = body[: max(0, height - 2)]
    while len(body) < max(0, height - 2):
        body.append("")
    rendered = [top]
    for line in body:
        rendered.append("|" + shorten(line, inner_width).ljust(inner_width) + "|")
    rendered.append("+" + ("-" * inner_width) + "+")
    return rendered


def merge_columns(left: list[str], right: list[str], gap: int = 1) -> list[str]:
    width_left = len(left[0]) if left else 0
    width_right = len(right[0]) if right else 0
    rows = max(len(left), len(right))
    out: list[str] = []
    for idx in range(rows):
        left_line = left[idx] if idx < len(left) else (" " * width_left)
        right_line = right[idx] if idx < len(right) else (" " * width_right)
        out.append(left_line + (" " * gap) + right_line)
    return out


def bundle_source_label(bundle: dict[str, Any]) -> str:
    source = bundle.get("source", {})
    if not isinstance(source, dict):
        return "unknown"
    mode = str(source.get("mode", "unknown"))
    if mode == "serial":
        return f"serial:{source.get('port', '?')}@{source.get('baud', '?')}"
    if mode == "log":
        path = str(source.get("path", "?"))
        return f"log:{Path(path).name}"
    path = str(source.get("path", ""))
    if path != "":
        return f"{mode}:{Path(path).name}"
    return mode


def stage_progress_lines(bundle: dict[str, Any], selected_action: str) -> list[str]:
    snapshot = bundle.get("snapshot", {})
    stage = bundle.get("stage", {})
    dependency = bundle.get("dependency", {})
    lines: list[str] = []

    stage_rows = stage.get("stage_json", []) if isinstance(stage, dict) else []
    wait_rows = stage.get("wait_json", []) if isinstance(stage, dict) else []
    stage_summary = get_first_row(stage_rows, "bringup-stage-summary")
    wait_summary = get_first_row(wait_rows, "bringup-stage-wait-summary")
    stage_items = get_rows_by_type(stage_rows, "bringup-stage")
    wait_drivers = get_rows_by_type(wait_rows, "bringup-stage-wait-driver")
    wait_resources = get_rows_by_type(wait_rows, "bringup-stage-wait-resource")
    bringup = snapshot.get("bringup", {}) if isinstance(snapshot, dict) else {}

    lines.append(f"source: {bundle_source_label(bundle)}")
    lines.append(
        "target: {target} | action: {action}".format(
            target=dependency.get("target_driver", "unknown"),
            action=selected_action,
        )
    )
    lines.append("")
    lines.append(
        "boot_complete={boot} active={active} last_error={err}".format(
            boot=bringup.get("boot_complete", "?"),
            active=bringup.get("active", "unknown"),
            err=bringup.get("last_error", "?"),
        )
    )

    if stage_summary:
        lines.append(
            "stage={stage} status={status} blocked={blocked}".format(
                stage=stage_summary.get("current", "unknown"),
                status=stage_summary.get("status", "unknown"),
                blocked=stage_summary.get("blocked", "?"),
            )
        )
    if wait_summary:
        lines.append(
            "wait phase={phase} waiting={waiting}/{targets}".format(
                phase=wait_summary.get("phase", "unknown"),
                waiting=wait_summary.get("waiting", "?"),
                targets=wait_summary.get("targets", "?"),
            )
        )

    lines.append("")
    lines.append("stage timeline:")
    if stage_items:
        current_stage = str(stage_summary.get("current", "")) if stage_summary else ""
        for row in stage_items:
            marker = "*" if str(row.get("name", "")) == current_stage else " "
            lines.append(
                "{marker} {name:<8} st={status:<11} fail={fails} rb={rollbacks}".format(
                    marker=marker,
                    name=str(row.get("name", "unknown"))[:8],
                    status=str(row.get("status", "unknown"))[:11],
                    fails=row.get("fails", 0),
                    rollbacks=row.get("rollbacks", 0),
                )
            )
    else:
        lines.append("no stage-json captured")

    lines.append("")
    lines.append("wait chain:")
    if wait_drivers:
        for row in wait_drivers[:3]:
            lines.append(
                "drv={driver} st={state} reason={reason}".format(
                    driver=row.get("driver", "unknown"),
                    state=row.get("driver_state", "unknown"),
                    reason=row.get("reason", "unknown"),
                )
            )
        for row in wait_resources[:3]:
            lines.append(
                "res={driver}:{kind}/{resource}".format(
                    driver=row.get("driver", "unknown"),
                    kind=row.get("kind", "unknown"),
                    resource=row.get("resource", "unknown"),
                )
            )
    else:
        lines.append("no pending driver/resource waits")
    return lines


def message_lines(messages: list[str], selected_action: str, selected_slice: int, slice_count: int) -> list[str]:
    lines: list[str] = []
    lines.append("recent activity:")
    if messages:
        for item in messages[-8:]:
            lines.append(f"- {item}")
    else:
        lines.append("- no messages yet")

    lines.append("")
    lines.append("keys:")
    lines.append("g refresh live bundle")
    lines.append("a toggle auto refresh")
    lines.append("+/- adjust auto interval")
    lines.append("1/2/3 select reset/throttle/deny report")
    lines.append("[ or ] cycle fault slice")
    lines.append("h show this help summary")
    lines.append("q quit")
    lines.append("")
    lines.append(f"slice={selected_slice + 1}/{max(1, slice_count)}")
    lines.append(f"what-if={selected_action}")
    return lines


def selected_fault_slice(bundle: dict[str, Any], selected_index: int) -> tuple[dict[str, Any], int, int]:
    snapshot = bundle.get("snapshot", {})
    fault_slices = snapshot.get("fault_slices", []) if isinstance(snapshot, dict) else []
    if not isinstance(fault_slices, list) or not fault_slices:
        return {}, 0, 0
    index = max(0, min(selected_index, len(fault_slices) - 1))
    obj = fault_slices[index]
    return obj if isinstance(obj, dict) else {}, index, len(fault_slices)


def report_lines(bundle: dict[str, Any], selected_action: str, selected_index: int) -> list[str]:
    dependency = bundle.get("dependency", {})
    slice_obj, resolved_index, slice_count = selected_fault_slice(bundle, selected_index)
    lines: list[str] = []

    if not slice_obj:
        lines.append("no fault slice captured in current bundle")
        lines.append("")
        lines.append("use a live port refresh or load a bundle with fault slices")
    else:
        meta = slice_obj.get("meta", {})
        feature = slice_obj.get("feature_vector", {})
        failure = slice_obj.get("failure", {})
        failure_explain = slice_obj.get("failure_explain", {})
        ai_failure = slice_obj.get("ai_failure", {})
        hypotheses = slice_obj.get("hypotheses", [])
        ai_support = slice_obj.get("ai_support", [])
        ai_checkpoints = slice_obj.get("ai_checkpoints", [])
        events = slice_obj.get("events", [])

        lines.append(
            "slice {idx}/{count} corr={corr} stage={stage} driver={driver}".format(
                idx=resolved_index + 1,
                count=max(1, slice_count),
                corr=meta.get("corr_id", "unknown"),
                stage=meta.get("stage", "unknown"),
                driver=feature.get("driver", ai_failure.get("driver", "unknown")),
            )
        )
        lines.append(
            "failure={failure} conf={conf}% events={events} fault={fault} reset={reset}".format(
                failure=failure.get("category", "unknown"),
                conf=failure.get("confidence", 0),
                events=meta.get("events", 0),
                fault=meta.get("fault_events", 0),
                reset=meta.get("reset_events", 0),
            )
        )
        lines.append(
            "features irq_abn={irq} dma_full={dma} kdi_fail={kdi} loop={loop}".format(
                irq=feature.get("irq_abnormal", 0),
                dma=feature.get("dma_full", 0),
                kdi=feature.get("kdi_fail_frequent", 0),
                loop=feature.get("state_error_reset_loop", 0),
            )
        )
        if hypotheses:
            hyp = hypotheses[0]
            lines.append(
                "hypothesis={name} conf={conf}% evidence={evidence}".format(
                    name=hyp.get("name", "unknown"),
                    conf=hyp.get("confidence", 0),
                    evidence=evidence_text(hyp.get("evidence_ids", [])),
                )
            )
        if failure_explain:
            lines.append("explanation:")
            if failure_explain.get("p1"):
                lines.append(f"- {failure_explain['p1']}")
            if failure_explain.get("p2"):
                lines.append(f"- {failure_explain['p2']}")

        if ai_checkpoints:
            lines.append("checkpoints:")
            for row in ai_checkpoints[:2]:
                lines.append(
                    "{step}. {cmd} | {focus}".format(
                        step=row.get("step", "?"),
                        cmd=row.get("cmd", "unknown"),
                        focus=row.get("focus", "unknown"),
                    )
                )

    whatif_map = dependency.get("whatif", {}) if isinstance(dependency, dict) else {}
    selected = whatif_map.get(selected_action, {}) if isinstance(whatif_map, dict) else {}
    summary = selected.get("summary", {}) if isinstance(selected, dict) else {}
    drivers = selected.get("drivers", []) if isinstance(selected, dict) else []
    stages = selected.get("stages", []) if isinstance(selected, dict) else []
    secondary = selected.get("secondary", []) if isinstance(selected, dict) else []

    lines.append(
        "{action} what-if target={target} affected_drivers={drivers_count} affected_stages={stages_count}".format(
            action=selected_action,
            target=dependency.get("target_driver", "unknown"),
            drivers_count=summary.get("affected_drivers", 0),
            stages_count=summary.get("affected_stages", 0),
        )
    )
    if drivers:
        lines.append("impact drivers:")
        for row in drivers[:4]:
            lines.append(
                "drv={driver} depth={depth} via={via} effect={effect}".format(
                    driver=row.get("driver", "unknown"),
                    depth=row.get("depth", "?"),
                    via=row.get("via", "none"),
                    effect=row.get("effect", "unknown"),
                )
            )
    else:
        lines.append("impact drivers: none")

    if stages:
        lines.append("impact stages:")
        for row in stages[:3]:
            lines.append(
                "stage={stage} phase={phase} via={driver} reason={reason}".format(
                    stage=row.get("stage", "unknown"),
                    phase=row.get("phase", "unknown"),
                    driver=row.get("driver", "unknown"),
                    reason=row.get("reason", "unknown"),
                )
            )
    if secondary:
        lines.append("secondary chains:")
        for row in secondary[:2]:
            lines.append(
                "drv={driver} middle={middle} upstream={upstream}".format(
                    driver=row.get("driver", "unknown"),
                    middle=row.get("middle", "unknown"),
                    upstream=row.get("upstream", "unknown"),
                )
            )

    if slice_obj and (ai_support or events):
        lines.append("supporting events:")
        if ai_support:
            for row in ai_support[:2]:
                lines.append(
                    "event={event} stage={stage} msg={msg}".format(
                        event=row.get("event_id", "?"),
                        stage=row.get("stage", "unknown"),
                        msg=row.get("msg", "unknown"),
                    )
                )
        else:
            for row in events[:2]:
                lines.append(
                    "event={event} flags={flags} msg={msg}".format(
                        event=row.get("event_id", "?"),
                        flags=row.get("flags", "unknown"),
                        msg=row.get("msg", "unknown"),
                    )
                )
    return lines


def header_lines(bundle: dict[str, Any]) -> list[str]:
    snapshot = bundle.get("snapshot", {})
    source = bundle_source_label(bundle)
    fault_slices = snapshot.get("fault_slices", []) if isinstance(snapshot, dict) else []
    events_window = snapshot.get("events_window", {}) if isinstance(snapshot, dict) else {}
    return [
        "MicroKernel-MPU Bringup Console",
        "Hardware-first terminal dashboard for bringup, dependency, and fault triage",
        f"source={source} fault_slices={len(fault_slices)} window_ms={events_window.get('window_ms', '?')}",
    ]


def footer_line(bundle: dict[str, Any], last_refresh: str) -> str:
    return footer_line_with_status(bundle, last_refresh, "off", "0.0s", "manual")


def footer_line_with_status(
    bundle: dict[str, Any],
    last_refresh: str,
    auto_refresh_label: str,
    age_label: str,
    freshness_label: str,
) -> str:
    dependency = bundle.get("dependency", {})
    target = dependency.get("target_driver", "unknown") if isinstance(dependency, dict) else "unknown"
    return (
        f"tgt={target} rf={last_refresh} auto={auto_refresh_label} age={age_label} {freshness_label} | "
        "g refresh a auto +/- rate 1/2/3 [ ] q"
    )


def render_dashboard(
    bundle: dict[str, Any],
    width: int,
    height: int,
    selected_action: str,
    selected_index: int,
    messages: list[str],
    last_refresh: str,
    auto_refresh_label: str = "off",
    age_label: str = "0.0s",
    freshness_label: str = "manual",
) -> list[str]:
    if width < MIN_WIDTH or height < MIN_HEIGHT:
        return [
            "terminal too small",
            f"need at least {MIN_WIDTH}x{MIN_HEIGHT}, got {width}x{height}",
        ]

    header_h = 5
    footer_h = 3
    gap_lines = 3
    body_h = height - header_h - footer_h - gap_lines
    top_h = max(12, body_h // 3)
    report_h = body_h - top_h - 1
    left_w = max(40, (width * 5) // 12)
    right_w = width - left_w - 1

    header = render_panel("MicroKernel-MPU", header_lines(bundle), width, header_h)
    progress = render_panel("Progress", stage_progress_lines(bundle, selected_action), left_w, top_h)
    faults = bundle.get("snapshot", {}).get("fault_slices", []) if isinstance(bundle.get("snapshot", {}), dict) else []
    tools = render_panel(
        "Messages & Tools",
        message_lines(messages, selected_action, selected_index, len(faults) if isinstance(faults, list) else 0),
        right_w,
        top_h,
    )
    report = render_panel("Current Report", report_lines(bundle, selected_action, selected_index), width, report_h)
    footer = render_panel(
        "Status",
        [footer_line_with_status(bundle, last_refresh, auto_refresh_label, age_label, freshness_label)],
        width,
        footer_h,
    )

    lines: list[str] = []
    lines.extend(header)
    lines.append("")
    lines.extend(merge_columns(progress, tools, gap=1))
    lines.append("")
    lines.extend(report)
    lines.append("")
    lines.extend(footer)
    return lines[:height]


class BringupUIDashboard:
    def __init__(self, ns: argparse.Namespace):
        self.ns = ns
        self.bundle: dict[str, Any] = {}
        self.selected_action = "reset"
        self.selected_index = 0
        self.last_refresh = "not-yet"
        self.messages: list[str] = []
        self.auto_refresh_s = nearest_auto_refresh(ns.auto_refresh_s)
        self.last_refresh_monotonic = 0.0
        self.next_refresh_deadline = 0.0

    def add_message(self, message: str) -> None:
        timestamp = time.strftime("%H:%M:%S")
        self.messages.append(f"{timestamp} {message}")
        if len(self.messages) > 20:
            self.messages = self.messages[-20:]

    def load_bundle(self) -> None:
        started = time.time()
        started_mono = time.monotonic()
        if self.ns.bundle_json is not None:
            self.bundle = load_bundle_json(Path(self.ns.bundle_json))
            self.add_message(f"loaded bundle json {Path(self.ns.bundle_json).name}")
        elif self.ns.source_log is not None:
            bundle, _ = triage_bundle.build_bundle_log(Path(self.ns.source_log), self.ns.target_driver)
            self.bundle = bundle
            self.add_message(f"loaded source log {Path(self.ns.source_log).name}")
        else:
            bundle, _ = triage_bundle.build_bundle_serial(
                self.ns.port,
                self.ns.baud,
                self.ns.char_delay_ms,
                self.ns.snapshot_timeout_s,
                self.ns.target_driver,
            )
            self.bundle = bundle
            self.add_message(f"captured live bundle from {self.ns.port}")
        elapsed = time.time() - started
        self.last_refresh = f"{elapsed:.2f}s"
        self.last_refresh_monotonic = time.monotonic()
        if self.auto_refresh_s > 0.0:
            self.next_refresh_deadline = started_mono + self.auto_refresh_s
        else:
            self.next_refresh_deadline = 0.0
        _, self.selected_index, count = selected_fault_slice(self.bundle, self.selected_index)
        if count == 0:
            self.selected_index = 0

    def auto_refresh_label(self) -> str:
        return fmt_seconds(self.auto_refresh_s)

    def age_seconds(self) -> float:
        if self.last_refresh_monotonic <= 0.0:
            return 0.0
        return max(0.0, time.monotonic() - self.last_refresh_monotonic)

    def age_label(self) -> str:
        return f"{self.age_seconds():.1f}s"

    def freshness_label(self) -> str:
        if self.auto_refresh_s <= 0.0:
            return "manual"
        age = self.age_seconds()
        if age <= self.auto_refresh_s * 1.2:
            return "fresh"
        if age <= self.auto_refresh_s * 2.0:
            return "aging"
        return "stale"

    def set_auto_refresh(self, interval_s: float) -> None:
        self.auto_refresh_s = nearest_auto_refresh(interval_s)
        if self.auto_refresh_s > 0.0:
            self.next_refresh_deadline = time.monotonic() + self.auto_refresh_s
        else:
            self.next_refresh_deadline = 0.0
        self.add_message(f"auto refresh {self.auto_refresh_label()}")

    def toggle_auto_refresh(self) -> None:
        if self.auto_refresh_s > 0.0:
            self.auto_refresh_s = 0.0
            self.next_refresh_deadline = 0.0
            self.add_message("auto refresh off")
            return
        self.set_auto_refresh(5.0)

    def step_auto_refresh(self, delta: int) -> None:
        self.set_auto_refresh(next_auto_refresh(self.auto_refresh_s, delta))

    def should_auto_refresh(self) -> bool:
        return self.auto_refresh_s > 0.0 and time.monotonic() >= self.next_refresh_deadline

    def cycle_slice(self, delta: int) -> None:
        snapshot = self.bundle.get("snapshot", {})
        slices = snapshot.get("fault_slices", []) if isinstance(snapshot, dict) else []
        if not isinstance(slices, list) or not slices:
            return
        self.selected_index = (self.selected_index + delta) % len(slices)
        self.add_message(f"selected fault slice {self.selected_index + 1}/{len(slices)}")

    def set_action(self, action: str) -> None:
        self.selected_action = action
        self.add_message(f"selected what-if action {action}")

    def render_once(self) -> str:
        return "\n".join(
            render_dashboard(
                self.bundle,
                self.ns.width,
                self.ns.height,
                self.selected_action,
                self.selected_index,
                self.messages,
                self.last_refresh,
                self.auto_refresh_label(),
                self.age_label(),
                self.freshness_label(),
            )
        )

    def run_curses(self) -> int:
        return curses.wrapper(self._curses_main)

    def _curses_main(self, stdscr: Any) -> int:
        curses.curs_set(0)
        stdscr.keypad(True)
        stdscr.timeout(250)
        if curses.has_colors():
            curses.start_color()
            curses.use_default_colors()
            curses.init_pair(1, curses.COLOR_GREEN, -1)
            curses.init_pair(2, curses.COLOR_CYAN, -1)
            curses.init_pair(3, curses.COLOR_YELLOW, -1)

        self._draw(stdscr)
        while True:
            ch = stdscr.getch()
            if ch == -1 and self.should_auto_refresh():
                self.add_message("auto refresh bundle")
                self._draw(stdscr)
                try:
                    self.load_bundle()
                except Exception as exc:
                    self.add_message(f"auto refresh failed: {exc}")
                self._draw(stdscr)
                continue
            if ch in (ord("q"), ord("Q")):
                return 0
            if ch in (ord("g"), ord("G")):
                self.add_message("refreshing bundle")
                self._draw(stdscr)
                try:
                    self.load_bundle()
                except Exception as exc:
                    self.add_message(f"refresh failed: {exc}")
            elif ch in (ord("a"), ord("A")):
                self.toggle_auto_refresh()
            elif ch in (ord("+"), ord("=")):
                self.step_auto_refresh(1)
            elif ch == ord("-"):
                self.step_auto_refresh(-1)
            elif ch == ord("1"):
                self.set_action("reset")
            elif ch == ord("2"):
                self.set_action("throttle")
            elif ch == ord("3"):
                self.set_action("deny")
            elif ch in (ord("["), curses.KEY_LEFT):
                self.cycle_slice(-1)
            elif ch in (ord("]"), curses.KEY_RIGHT):
                self.cycle_slice(1)
            elif ch in (ord("h"), ord("H")):
                self.add_message("help: g refresh, a auto, +/- rate, 1/2/3 action, [ ] slice, q quit")
            self._draw(stdscr)

    def _draw(self, stdscr: Any) -> None:
        stdscr.erase()
        height, width = stdscr.getmaxyx()
        lines = render_dashboard(
            self.bundle,
            width,
            height,
            self.selected_action,
            self.selected_index,
            self.messages,
            self.last_refresh,
            self.auto_refresh_label(),
            self.age_label(),
            self.freshness_label(),
        )
        for row_idx, line in enumerate(lines[:height]):
            attr = 0
            if curses.has_colors():
                if row_idx == 0 or line.startswith("+") or " Status " in line or " MicroKernel-MPU " in line:
                    attr = curses.color_pair(1)
                elif " Messages & Tools " in line or " Progress " in line:
                    attr = curses.color_pair(2)
                elif " Current Report " in line:
                    attr = curses.color_pair(3)
            stdscr.addnstr(row_idx, 0, line, max(0, width - 1), attr)
        stdscr.refresh()


def main() -> int:
    ns = parse_args()
    app = BringupUIDashboard(ns)
    try:
        app.load_bundle()
    except Exception as exc:
        raise SystemExit(f"bringup-ui error: {exc}") from exc

    if ns.render_once:
        print(app.render_once())
        return 0
    return app.run_curses()


if __name__ == "__main__":
    raise SystemExit(main())
