#!/usr/bin/env python3
import argparse
import json
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any

try:
    import serial  # type: ignore
except Exception as exc:
    raise SystemExit("pyserial not installed. Run: pip3 install pyserial") from exc

from regress_common import OPENOCD_CFG, reset_board

ROOT = Path(__file__).resolve().parent.parent
LOG_DIR = ROOT / "logs"
COMPARE_TOOL = ROOT / "tools" / "driver_profile_compare.py"


@dataclass
class Step:
    name: str
    command: str
    timeout_s: float
    must_contain: tuple[str, ...]


def read_for(ser: serial.Serial, seconds: float) -> str:
    end = time.time() + seconds
    out: list[str] = []
    while time.time() < end:
        n = ser.in_waiting
        data = ser.read(n or 1)
        if data:
            out.append(data.decode("utf-8", "replace"))
    return "".join(out)


def sync_cli(ser: serial.Serial) -> None:
    for _ in range(5):
        ser.write(b"\r")
        time.sleep(0.03)
    _ = read_for(ser, 0.35)


def send_line(ser: serial.Serial, line: str, char_delay_s: float) -> None:
    for ch in (line + "\r"):
        ser.write(ch.encode("utf-8"))
        if char_delay_s > 0:
            time.sleep(char_delay_s)


def normalize_text(text: str) -> str:
    return text.replace("\r\n", "\n").replace("\r", "\n")


def run_step(ser: serial.Serial, step: Step, char_delay_s: float, retries: int) -> dict[str, Any]:
    outputs: list[str] = []
    attempts = max(retries, 0) + 1

    for attempt in range(1, attempts + 1):
        sync_cli(ser)
        send_line(ser, step.command, char_delay_s)
        output = read_for(ser, step.timeout_s)
        output += read_for(ser, 0.20)
        outputs.append(output)
        ok = all(token in output for token in step.must_contain)
        if ok:
            return {
                "name": step.name,
                "command": step.command,
                "ok": True,
                "attempt": attempt,
                "output": output,
                "attempt_outputs": outputs,
            }

    return {
        "name": step.name,
        "command": step.command,
        "ok": False,
        "attempt": attempts,
        "output": outputs[-1] if outputs else "",
        "attempt_outputs": outputs,
    }


def build_steps() -> list[Step]:
    steps: list[Step] = [
        Step("disable_logs", "disable", 0.9, ("status:",)),
        Step("profile_reset", "profile reset", 1.0, ("profile reset: ok",)),
    ]
    for i in range(8):
        steps.append(
            Step(
                f"irq_storm_{i}",
                "kdi irq storm uart 180",
                2.4,
                ("kdi irq storm uart",),
            )
        )
        steps.append(
            Step(
                f"irq_worker_{i}",
                "kdi irq worker 256",
                1.4,
                ("kdi irq worker", "processed="),
            )
        )
    steps.extend(
        [
            Step("new_cap_usage_sensor", "kdi probe deny", 1.0, ("kdi probe deny:",)),
            Step("uart_error", "kdi driver error uart 85", 1.0, ("kdi driver error uart: ok",)),
            Step("uart_reset", "kdi driver reset uart", 1.0, ("kdi driver reset uart: ok",)),
            Step("uart_reinit", "kdi driver reinit uart", 1.0, ("kdi driver reinit uart: ok",)),
            Step("uart_probe", "kdi driver probe uart", 1.0, ("kdi driver probe uart: ok",)),
            Step("uart_ready", "kdi driver ready uart", 1.0, ("kdi driver ready uart: ok",)),
            Step("uart_active", "kdi driver active uart", 1.0, ("kdi driver active uart: ok",)),
        ]
    )
    return steps


def make_log_path() -> Path:
    LOG_DIR.mkdir(parents=True, exist_ok=True)
    ts = time.strftime("%Y%m%d_%H%M%S")
    return LOG_DIR / f"profile_compare_regress_{ts}.log"


def write_log(log_path: Path, payload: dict[str, Any]) -> None:
    lines: list[str] = []
    lines.append(f"time={payload['time']}")
    lines.append(f"port={payload['port']} baud={payload['baud']}")
    lines.append(f"reset={payload['reset']}")
    lines.append(f"profile_file={payload['profile_file']}")
    lines.append(f"compare_cmd={' '.join(payload['compare_cmd'])}")
    lines.append(f"compare_rc={payload['compare_rc']}")
    lines.append("")
    lines.append("== baseline_profile ==")
    lines.append(normalize_text(payload["baseline_profile"]))
    lines.append("")
    for item in payload["steps"]:
        lines.append(f"== {item['name']} ({'PASS' if item['ok'] else 'FAIL'}) ==")
        lines.append(f"cmd: {item['command']}")
        lines.append(f"attempt: {item.get('attempt', 1)}")
        lines.append("output:")
        lines.append(normalize_text(item["output"]))
        attempts = item.get("attempt_outputs")
        if isinstance(attempts, list) and len(attempts) > 1:
            lines.append("attempt_outputs:")
            for idx, attempt_output in enumerate(attempts, start=1):
                lines.append(f"-- try {idx} --")
                lines.append(normalize_text(str(attempt_output)))
        lines.append("")
    lines.append("== candidate_profile ==")
    lines.append(normalize_text(payload["candidate_profile"]))
    lines.append("")
    lines.append("== compare_stdout ==")
    lines.append(normalize_text(payload["compare_stdout"]))
    lines.append("")
    lines.append("== compare_stderr ==")
    lines.append(normalize_text(payload["compare_stderr"]))
    lines.append("")
    lines.append(f"overall={'PASS' if payload['ok'] else 'FAIL'}")
    log_path.write_text("\n".join(lines), encoding="utf-8")


def find_driver_result(report: dict[str, Any], driver: str) -> dict[str, Any] | None:
    for item in report.get("results", []):
        if item.get("driver") == driver:
            return item
    return None


def run_compare(profile_file: Path) -> tuple[int, str, str, dict[str, Any] | None, list[str]]:
    cmd = [
        sys.executable,
        str(COMPARE_TOOL),
        "--baseline",
        str(profile_file),
        "--candidate",
        str(profile_file),
        "--baseline-index",
        "0",
        "--candidate-index",
        "1",
        "--period-compare",
        "--driver",
        "uart",
        "--driver",
        "sensor",
        "--dist-shift-threshold-pct",
        "2",
        "--dist-new-bin-pct",
        "2",
        "--jitter-min-delta",
        "1",
        "--jitter-rate-delta-pct",
        "1",
        "--loop-min-delta",
        "1",
        "--baseline-label",
        "period_1",
        "--candidate-label",
        "period_2",
        "--json",
    ]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    errors: list[str] = []
    report = None
    if proc.returncode != 0:
        errors.append(f"compare_rc={proc.returncode}")
        return proc.returncode, proc.stdout, proc.stderr, None, errors

    try:
        report = json.loads(proc.stdout)
    except Exception:
        errors.append("compare_json_parse_failed")
        return proc.returncode, proc.stdout, proc.stderr, None, errors

    uart = find_driver_result(report, "uart")
    sensor = find_driver_result(report, "sensor")
    if uart is None:
        errors.append("missing_uart_result")
    if sensor is None:
        errors.append("missing_sensor_result")

    if uart is not None:
        uart_types = {a.get("type", "") for a in uart.get("anomalies", [])}
        if not uart.get("regression", False):
            errors.append("uart_not_regression")
        if "distribution_shift" not in uart_types:
            errors.append("uart_no_distribution_shift")
        if "state_jitter_increase" not in uart_types:
            errors.append("uart_no_state_jitter_increase")

    if sensor is not None:
        sensor_types = {a.get("type", "") for a in sensor.get("anomalies", [])}
        if not sensor.get("regression", False):
            errors.append("sensor_not_regression")
        if "new_capability_usage" not in sensor_types:
            errors.append("sensor_no_new_capability_usage")

    return proc.returncode, proc.stdout, proc.stderr, report, errors


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Run on-board driver profile baseline/period comparison regression workflow."
    )
    ap.add_argument("--port", required=True, help="serial port, e.g. /dev/cu.usbmodemXXXX")
    ap.add_argument("--baud", type=int, default=115200, help="UART baud rate")
    ap.add_argument("--char-delay-ms", type=float, default=10.0, help="per-char TX delay in ms")
    ap.add_argument("--retries", type=int, default=1, help="retry count per step on mismatch")
    ap.add_argument("--no-reset", action="store_true", help="skip OpenOCD reset before test")
    ap.add_argument("--json", action="store_true", help="emit machine-readable JSON summary")
    ns = ap.parse_args()

    if not COMPARE_TOOL.exists():
        print(f"error: compare tool not found: {COMPARE_TOOL}", file=sys.stderr)
        return 2

    if not ns.no_reset:
        if not OPENOCD_CFG.exists():
            print(f"error: OpenOCD config not found: {OPENOCD_CFG}", file=sys.stderr)
            return 2
        try:
            reset_board()
        except Exception as exc:
            print(f"error: {exc}", file=sys.stderr)
            return 2

    steps = build_steps()
    results: list[dict[str, Any]] = []
    baseline_profile = ""
    candidate_profile = ""
    char_delay_s = ns.char_delay_ms / 1000.0

    try:
        ser = serial.Serial(ns.port, ns.baud, timeout=0.03)
    except Exception as exc:
        print(f"error: open serial failed: {exc}", file=sys.stderr)
        return 2

    try:
        _ = read_for(ser, 1.0)
        sync_cli(ser)

        # Baseline capture window: reset profile runtime and allow a short idle window.
        disable_step = run_step(ser, steps[0], char_delay_s, ns.retries)
        results.append(disable_step)
        if not disable_step["ok"]:
            raise RuntimeError("disable step failed")

        reset_step = run_step(ser, steps[1], char_delay_s, ns.retries)
        results.append(reset_step)
        if not reset_step["ok"]:
            raise RuntimeError("profile reset step failed")

        _ = read_for(ser, 2.2)
        sync_cli(ser)
        send_line(ser, "profile json", char_delay_s)
        baseline_profile = read_for(ser, 3.6) + read_for(ser, 0.25)
        if "driver-profile-meta" not in baseline_profile:
            raise RuntimeError("baseline profile snapshot missing")

        # Candidate workload + capture.
        for step in steps[2:]:
            result = run_step(ser, step, char_delay_s, ns.retries)
            results.append(result)
            if not result["ok"]:
                break
            if step.name.startswith("irq_worker_"):
                _ = read_for(ser, 0.55)

        if all(item["ok"] for item in results):
            sync_cli(ser)
            send_line(ser, "profile json", char_delay_s)
            candidate_profile = read_for(ser, 3.6) + read_for(ser, 0.25)
            if "driver-profile-meta" not in candidate_profile:
                raise RuntimeError("candidate profile snapshot missing")
    except Exception as exc:
        payload = {
            "ok": False,
            "time": time.strftime("%Y-%m-%d %H:%M:%S"),
            "port": ns.port,
            "baud": ns.baud,
            "reset": not ns.no_reset,
            "profile_file": "",
            "compare_cmd": [],
            "compare_rc": -1,
            "baseline_profile": baseline_profile,
            "candidate_profile": candidate_profile,
            "steps": results,
            "compare_stdout": "",
            "compare_stderr": str(exc),
        }
        log_path = make_log_path()
        write_log(log_path, payload)
        payload["log"] = str(log_path)
        if ns.json:
            print(json.dumps(payload, ensure_ascii=True))
        else:
            print(f"error: {exc}", file=sys.stderr)
            print(f"log: {log_path}")
            print("overall: FAIL")
        return 1
    finally:
        ser.close()

    ts = time.strftime("%Y%m%d_%H%M%S")
    profile_file = LOG_DIR / f"profile_compare_capture_{ts}.jsonl"
    LOG_DIR.mkdir(parents=True, exist_ok=True)
    profile_file.write_text(
        "\n".join(
            [
                normalize_text(baseline_profile).strip(),
                normalize_text(candidate_profile).strip(),
            ]
        )
        + "\n",
        encoding="utf-8",
    )

    compare_rc, compare_stdout, compare_stderr, compare_report, compare_errors = run_compare(
        profile_file
    )
    compare_cmd = [
        str(COMPARE_TOOL),
        "--baseline",
        str(profile_file),
        "--candidate",
        str(profile_file),
        "--baseline-index",
        "0",
        "--candidate-index",
        "1",
        "--period-compare",
        "--driver",
        "uart",
        "--driver",
        "sensor",
        "--dist-shift-threshold-pct",
        "2",
        "--dist-new-bin-pct",
        "2",
        "--jitter-min-delta",
        "1",
        "--jitter-rate-delta-pct",
        "1",
        "--loop-min-delta",
        "1",
        "--baseline-label",
        "period_1",
        "--candidate-label",
        "period_2",
        "--json",
    ]

    ok_steps = all(item["ok"] for item in results) and len(results) == len(steps)
    ok_compare = compare_rc == 0 and len(compare_errors) == 0
    ok = ok_steps and ok_compare

    payload = {
        "ok": ok,
        "time": time.strftime("%Y-%m-%d %H:%M:%S"),
        "port": ns.port,
        "baud": ns.baud,
        "reset": not ns.no_reset,
        "profile_file": str(profile_file),
        "compare_cmd": compare_cmd,
        "compare_rc": compare_rc,
        "baseline_profile": baseline_profile,
        "candidate_profile": candidate_profile,
        "steps": results,
        "compare_stdout": compare_stdout,
        "compare_stderr": compare_stderr + ("\n" + ";".join(compare_errors) if compare_errors else ""),
    }
    log_path = make_log_path()
    write_log(log_path, payload)
    payload["log"] = str(log_path)
    payload["compare_errors"] = compare_errors
    payload["compare_report"] = compare_report

    if ns.json:
        print(json.dumps(payload, ensure_ascii=True))
    else:
        for item in results:
            status = "PASS" if item["ok"] else "FAIL"
            print(f"[{status}] {item['command']}")
        if compare_rc == 0 and len(compare_errors) == 0:
            print("[PASS] profile compare classification")
        else:
            print("[FAIL] profile compare classification")
            if len(compare_errors) != 0:
                print("errors: " + ",".join(compare_errors))
        print(f"profile: {profile_file}")
        print(f"log: {log_path}")
        print(f"overall: {'PASS' if ok else 'FAIL'}")

    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
