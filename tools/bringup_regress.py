#!/usr/bin/env python3
import argparse
import json
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any

try:
    import serial  # type: ignore
except Exception as exc:
    raise SystemExit("pyserial not installed. Run: pip3 install pyserial") from exc

from regress_common import OPENOCD_CFG, rebuild_and_flash, reset_board

ROOT = Path(__file__).resolve().parent.parent
LOG_DIR = ROOT / "logs"


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


def read_until_tokens(ser: serial.Serial, timeout_s: float, done_tokens: tuple[str, ...]) -> str:
    end = time.time() + timeout_s
    out: list[str] = []
    joined = ""

    while time.time() < end:
        n = ser.in_waiting
        data = ser.read(n or 1)
        if not data:
            continue
        chunk = data.decode("utf-8", "replace")
        out.append(chunk)
        joined += chunk
        if done_tokens and all(token in joined for token in done_tokens):
            # Drain remaining buffered bytes and return.
            out.append(read_for(ser, 0.35))
            return "".join(out)

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
        if step.command == "snapshot":
            output = read_until_tokens(ser, step.timeout_s, ("snapshot end",))
        else:
            output = read_for(ser, step.timeout_s)
            output += read_for(ser, 0.20)
        outputs.append(output)
        ok = all(token in output for token in step.must_contain)
        if ok:
            return {
                "name": step.name,
                "command": step.command,
                "ok": True,
                "timeout_s": step.timeout_s,
                "must_contain": list(step.must_contain),
                "attempt": attempt,
                "output": output,
                "attempt_outputs": outputs,
            }

    return {
        "name": step.name,
        "command": step.command,
        "ok": False,
        "timeout_s": step.timeout_s,
        "must_contain": list(step.must_contain),
        "attempt": attempts,
        "output": outputs[-1] if outputs else "",
        "attempt_outputs": outputs,
    }


def build_preflight_steps() -> list[Step]:
    return [
        Step(
            "preflight_phase_show",
            "bringup phase show",
            1.2,
            ("bringup phase rom-early-init", "driver-probe-diag"),
        ),
        Step(
            "preflight_stage_show",
            "bringup stage show",
            1.2,
            (
                "bringup stage-summary",
                "bringup stage init",
                "entry_evt=bringup.stage.init.enter",
                "bringup stage-wait-summary",
            ),
        ),
    ]


def build_steps(snapshot_timeout_s: float) -> list[Step]:
    return [
        Step("disable_logs", "disable", 0.9, ("status:",)),
        Step("phase_reset", "bringup phase reset", 1.0, ("bringup phase reset: ok",)),
        Step(
            "stage_show_after_reset",
            "bringup stage show",
            1.2,
            (
                "bringup stage-summary boot_complete=0 current=init",
                "blocked=1",
                "bringup stage-wait-summary stage=init",
            ),
        ),
        Step(
            "stage_wait_after_reset",
            "bringup stage wait",
            1.2,
            ("bringup stage-wait-summary stage=init", "bringup stage-wait fallback"),
        ),
        Step("phase_run_full", "bringup phase run", 1.2, ("bringup phase run: ok",)),
        Step(
            "stage_show_full",
            "bringup stage show",
            1.3,
            (
                "bringup stage-summary boot_complete=1 current=ready",
                "blocked=0",
                "bringup stage drivers st=done",
                "entry_evt=bringup.stage.drivers.enter",
                "bringup stage-wait-summary stage=ready",
            ),
        ),
        Step(
            "phase_show_full",
            "bringup phase show",
            1.4,
            (
                "bringup phase-summary boot_complete=1",
                "bringup phase driver-probe-diag st=done",
                "bringup phase driver-probe-uart st=done",
                "bringup phase driver-probe-sensor st=done",
                "bringup phase driver-probe-vm st=done",
                "bringup phase user-workload-enable st=done",
            ),
        ),
        Step(
            "phase_inject_sensor",
            "bringup phase inject sensor -901",
            1.0,
            ("bringup phase inject driver-probe-sensor: ok code=-901",),
        ),
        Step(
            "phase_run_injected_fail",
            "bringup phase run",
            1.2,
            ("bringup phase run fail driver-probe-sensor: injected(-901)", "logical rollback applied"),
        ),
        Step(
            "phase_show_after_inject_fail",
            "bringup phase show",
            1.4,
            (
                "bringup phase-summary boot_complete=0",
                "bringup phase driver-probe-sensor st=rolled_back",
                "bringup phase driver-probe-vm st=pending",
            ),
        ),
        Step(
            "stage_show_after_inject_fail",
            "bringup stage show",
            1.3,
            (
                "bringup stage-summary boot_complete=0 current=drivers",
                "status=rolled_back",
                "blocked=1",
                "bringup stage-wait-summary stage=drivers",
            ),
        ),
        Step("phase_rerun_sensor", "bringup phase rerun sensor", 1.2, ("bringup phase run: ok",)),
        Step(
            "phase_show_after_rerun_sensor",
            "bringup phase show",
            1.4,
            (
                "bringup phase-summary boot_complete=1",
                "bringup phase driver-probe-vm st=done",
                "bringup phase service-registration st=done",
                "bringup phase user-workload-enable st=done",
            ),
        ),
        Step(
            "phase_rollback_service",
            "bringup phase rollback service",
            1.0,
            ("bringup phase rollback service-registration: changed=", "(logical)"),
        ),
        Step(
            "phase_show_after_service_rollback",
            "bringup phase show",
            1.4,
            (
                "bringup phase-summary boot_complete=0",
                "bringup phase service-registration st=rolled_back",
                "bringup phase user-workload-enable st=rolled_back",
            ),
        ),
        Step("phase_rerun_service", "bringup phase rerun service", 1.2, ("bringup phase run: ok",)),
        Step(
            "phase_show_final",
            "bringup phase show",
            1.4,
            (
                "bringup phase-summary boot_complete=1",
                "bringup phase service-registration st=done",
                "bringup phase user-workload-enable st=done",
            ),
        ),
        Step("phase_clearfail_all", "bringup phase clearfail all", 1.0, ("bringup phase clearfail all: ok",)),
        Step(
            "phase_json_smoke",
            "bringup phase json",
            1.4,
            ('{"type":"bringup-phase-summary"', '"name":"driver-probe-vm"'),
        ),
        Step(
            "stage_json_smoke",
            "bringup stage json",
            1.4,
            (
                '{"type":"bringup-stage-summary"',
                '"name":"drivers"',
                '"entry_event":"bringup.stage.drivers.enter"',
                '{"type":"bringup-stage-wait-summary"',
            ),
        ),
        Step(
            "stage_wait_json_smoke",
            "bringup stage wait-json",
            1.2,
            ('{"type":"bringup-stage-wait-summary"', '"stage":"ready"'),
        ),
        Step(
            "phase_inject_sensor_final",
            "bringup phase inject sensor -902",
            1.0,
            ("bringup phase inject driver-probe-sensor: ok code=-902",),
        ),
        Step(
            "phase_run_injected_fail_final",
            "bringup phase run",
            1.2,
            ("bringup phase run fail driver-probe-sensor: injected(-902)", "logical rollback applied"),
        ),
        Step(
            "snapshot_failure_classify",
            "snapshot",
            snapshot_timeout_s,
            (
                "snapshot failure slice=",
                "category=",
                "snapshot ai failure slice=",
                "snapshot ai checkpoint slice=",
                "snapshot ai cap-shrink",
            ),
        ),
    ]


def run_workflow(
    port: str,
    baud: int,
    char_delay_s: float,
    retries: int,
    preflight_steps: list[Step],
    steps: list[Step],
) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    preflight_results: list[dict[str, Any]] = []
    results: list[dict[str, Any]] = []

    try:
        ser = serial.Serial(port, baud, timeout=0.03)
    except Exception as exc:
        raise RuntimeError(f"open serial failed: {exc}") from exc

    try:
        _ = read_for(ser, 1.2)
        sync_cli(ser)
        send_line(ser, "disable", char_delay_s)
        _ = read_for(ser, 0.8)

        for step in preflight_steps:
            result = run_step(ser, step, char_delay_s, retries)
            preflight_results.append(result)
            if not result["ok"]:
                return preflight_results, results

        for step in steps:
            result = run_step(ser, step, char_delay_s, retries)
            results.append(result)
            if not result["ok"]:
                break
    finally:
        ser.close()

    return preflight_results, results


def make_log_path() -> Path:
    LOG_DIR.mkdir(parents=True, exist_ok=True)
    ts = time.strftime("%Y%m%d_%H%M%S")
    return LOG_DIR / f"bringup_regress_{ts}.log"


def write_log(log_path: Path, payload: dict[str, Any]) -> None:
    lines: list[str] = []
    lines.append(f"time={payload['time']}")
    lines.append(f"port={payload['port']} baud={payload['baud']}")
    lines.append(
        f"reset={payload['reset']} flashed={payload.get('flashed', False)} "
        f"auto_flash_stale={payload.get('auto_flash_stale', False)}"
    )
    lines.append("")

    preflight = payload.get("preflight", [])
    if isinstance(preflight, list):
        for item in preflight:
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
    lines.append(f"overall={'PASS' if payload['ok'] else 'FAIL'}")
    log_path.write_text("\n".join(lines), encoding="utf-8")


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Run on-board bring-up phase regression (dump/run/inject/rollback/rerun)."
    )
    ap.add_argument("--port", required=True, help="serial port, e.g. /dev/cu.usbmodemXXXX")
    ap.add_argument("--baud", type=int, default=115200, help="UART baud rate")
    ap.add_argument("--char-delay-ms", type=float, default=10.0, help="per-char TX delay in ms")
    ap.add_argument("--retries", type=int, default=2, help="retry count per step on mismatch")
    ap.add_argument(
        "--snapshot-timeout-s",
        type=float,
        default=45.0,
        help="timeout for snapshot step in seconds",
    )
    ap.add_argument("--no-reset", action="store_true", help="skip OpenOCD reset before test")
    ap.add_argument("--flash", action="store_true", help="build and flash firmware before test")
    ap.add_argument("--flash-skip-build", action="store_true", help="with --flash, skip build and only flash existing ELF")
    ap.add_argument(
        "--no-auto-flash-stale",
        action="store_true",
        help="do not auto flash when bringup preflight detects stale firmware",
    )
    ap.add_argument("--json", action="store_true", help="emit machine-readable JSON summary")
    ns = ap.parse_args()

    if not OPENOCD_CFG.exists():
        print(f"error: OpenOCD config not found: {OPENOCD_CFG}", file=sys.stderr)
        return 2

    if ns.snapshot_timeout_s <= 0.0:
        print("error: --snapshot-timeout-s must be > 0", file=sys.stderr)
        return 2

    steps = build_steps(ns.snapshot_timeout_s)
    preflight_steps = build_preflight_steps()
    char_delay_s = ns.char_delay_ms / 1000.0
    auto_flash_stale = not ns.no_auto_flash_stale
    flashed = False

    if ns.flash:
        try:
            rebuild_and_flash(skip_build=ns.flash_skip_build)
            flashed = True
        except Exception as exc:
            print(f"error: {exc}", file=sys.stderr)
            return 2

    if not ns.no_reset:
        try:
            reset_board()
        except Exception as exc:
            print(f"error: {exc}", file=sys.stderr)
            return 2

    try:
        preflight_results, results = run_workflow(
            ns.port, ns.baud, char_delay_s, ns.retries, preflight_steps, steps
        )
    except Exception as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    preflight_ok = all(item["ok"] for item in preflight_results)
    if not preflight_ok and auto_flash_stale and not flashed:
        print(
            "warning: bringup preflight failed; rebuilding and flashing firmware once",
            file=sys.stderr,
        )
        try:
            rebuild_and_flash(skip_build=ns.flash_skip_build)
            flashed = True
        except Exception as exc:
            print(f"error: {exc}", file=sys.stderr)
            return 2

        if not ns.no_reset:
            try:
                reset_board()
            except Exception as exc:
                print(f"error: {exc}", file=sys.stderr)
                return 2

        try:
            preflight_results, results = run_workflow(
                ns.port, ns.baud, char_delay_s, ns.retries, preflight_steps, steps
            )
        except Exception as exc:
            print(f"error: {exc}", file=sys.stderr)
            return 2
        preflight_ok = all(item["ok"] for item in preflight_results)

    if not preflight_ok:
        bad = next((item for item in preflight_results if not item["ok"]), None)
        command = bad["command"] if bad is not None else "bringup phase show"
        print(
            f"error: firmware preflight failed on '{command}'. "
            "Flash latest firmware or pass --flash.",
            file=sys.stderr,
        )
        return 2

    ok = all(item["ok"] for item in results) and len(results) == len(steps)
    payload = {
        "ok": ok,
        "time": time.strftime("%Y-%m-%d %H:%M:%S"),
        "port": ns.port,
        "baud": ns.baud,
        "reset": not ns.no_reset,
        "flashed": flashed,
        "auto_flash_stale": auto_flash_stale,
        "preflight": preflight_results,
        "steps_total": len(steps),
        "steps_executed": len(results),
        "steps": results,
    }

    log_path = make_log_path()
    write_log(log_path, payload)
    payload["log"] = str(log_path)

    if ns.json:
        print(json.dumps(payload, ensure_ascii=True))
    else:
        for item in preflight_results + results:
            status = "PASS" if item["ok"] else "FAIL"
            print(f"[{status}] {item['command']}")
        print(f"log: {log_path}")
        print(f"overall: {'PASS' if ok else 'FAIL'}")

    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
