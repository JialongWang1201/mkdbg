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

from regress_common import OPENOCD_CFG, reset_board

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


def build_steps() -> list[Step]:
    steps: list[Step] = [
        Step(
            name="disable_logs",
            command="disable",
            timeout_s=0.9,
            must_contain=("status:",),
        ),
        Step(
            name="sonic_cap",
            command="sonic cap",
            timeout_s=1.0,
            must_contain=("sonic cap db=", "sonic names=config,appl,asic"),
        ),
        Step(
            name="preset_list",
            command="sonic preset list",
            timeout_s=1.1,
            must_contain=("sonic preset count=", "sonic preset lab-safe:", "sonic preset enforce-prod:"),
        ),
        Step(
            name="preset_show_lab_safe",
            command="sonic preset show lab-safe",
            timeout_s=1.1,
            must_contain=("sonic preset lab-safe", "preset mig.mode=monitor", "preset vm.sample_ms=250"),
        ),

        # Case A: candidate apply must be atomic on db_full.
        Step(
            name="atomic_abort_start",
            command="sonic abort",
            timeout_s=0.9,
            must_contain=("sonic abort ok",),
        ),
    ]

    for i in range(12):
        steps.append(
            Step(
                name=f"atomic_fill_k{i}",
                command=f"sonic set config k{i} v{i}",
                timeout_s=0.8,
                must_contain=(f"sonic set staged config.k{i} dirty=1",),
            )
        )

    steps.extend([
        Step(
            name="atomic_apply_candidate_fail",
            command="sonic preset apply lab-safe candidate",
            timeout_s=1.3,
            must_contain=("sonic preset apply: db_full on mig.allow",),
        ),
        Step(
            name="atomic_mig_mode_not_staged",
            command="sonic get candidate config mig.mode",
            timeout_s=0.9,
            must_contain=("sonic get: not_found",),
        ),
        Step(
            name="atomic_mig_allow_not_staged",
            command="sonic get candidate config mig.allow",
            timeout_s=0.9,
            must_contain=("sonic get: not_found",),
        ),
        Step(
            name="atomic_existing_key_kept",
            command="sonic get candidate config k11",
            timeout_s=0.9,
            must_contain=("sonic candidate config k11=v11",),
        ),

        # Case B: candidate apply must not mutate runtime side-effects.
        Step(
            name="sidefx_abort_start",
            command="sonic abort",
            timeout_s=0.9,
            must_contain=("sonic abort ok",),
        ),
        Step(
            name="sidefx_mig_reset",
            command="vm mig reset",
            timeout_s=0.9,
            must_contain=("vm mig: mode=off",),
        ),
        Step(
            name="sidefx_mig_mode_off",
            command="vm mig mode off",
            timeout_s=0.9,
            must_contain=("vm mig: mode=off",),
        ),
        Step(
            name="sidefx_status_before",
            command="status",
            timeout_s=0.9,
            must_contain=("status: sample_ms=100", "log=off"),
        ),
        Step(
            name="sidefx_candidate_apply",
            command="sonic preset apply lab-safe candidate",
            timeout_s=1.2,
            must_contain=("sonic preset apply ok name=lab-safe view=candidate",),
        ),
        Step(
            name="sidefx_status_after",
            command="status",
            timeout_s=0.9,
            must_contain=("status: sample_ms=100", "log=off"),
        ),
        Step(
            name="sidefx_mig_unchanged",
            command="vm mig status",
            timeout_s=0.9,
            must_contain=("vm mig: mode=off",),
        ),
        Step(
            name="sidefx_candidate_staged",
            command="sonic get candidate config vm.sample_ms",
            timeout_s=0.9,
            must_contain=("sonic candidate config vm.sample_ms=250",),
        ),

        # Case C: running apply must not inherit unrelated candidate dirty keys.
        Step(
            name="running_iso_abort_start",
            command="sonic abort",
            timeout_s=0.9,
            must_contain=("sonic abort ok",),
        ),
        Step(
            name="running_iso_stage_scratch",
            command="sonic set config scratch leak",
            timeout_s=0.8,
            must_contain=("sonic set staged config.scratch dirty=1",),
        ),
        Step(
            name="running_iso_apply_running",
            command="sonic preset apply lab-safe running",
            timeout_s=1.4,
            must_contain=("sonic preset apply ok name=lab-safe view=running",),
        ),
        Step(
            name="running_iso_running_clean",
            command="sonic get running config scratch",
            timeout_s=0.9,
            must_contain=("sonic get: not_found",),
        ),
        Step(
            name="running_iso_candidate_clean",
            command="sonic get candidate config scratch",
            timeout_s=0.9,
            must_contain=("sonic get: not_found",),
        ),
        Step(
            name="running_iso_runtime_applied",
            command="vm mig status",
            timeout_s=0.9,
            must_contain=("vm mig: mode=monitor",),
        ),
    ])

    return steps


def make_log_path() -> Path:
    LOG_DIR.mkdir(parents=True, exist_ok=True)
    ts = time.strftime("%Y%m%d_%H%M%S")
    return LOG_DIR / f"sonic_regress_{ts}.log"


def write_log(log_path: Path, payload: dict[str, Any]) -> None:
    lines: list[str] = []
    lines.append(f"time={payload['time']}")
    lines.append(f"port={payload['port']} baud={payload['baud']}")
    lines.append(f"reset={payload['reset']}")
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
        description="Run on-board Sonic functional regression for preset/transaction behavior."
    )
    ap.add_argument("--port", required=True, help="serial port, e.g. /dev/cu.usbmodemXXXX")
    ap.add_argument("--baud", type=int, default=115200, help="UART baud rate")
    ap.add_argument("--char-delay-ms", type=float, default=10.0, help="per-char TX delay in ms")
    ap.add_argument("--retries", type=int, default=2, help="retry count per step on mismatch")
    ap.add_argument("--no-reset", action="store_true", help="skip OpenOCD reset before test")
    ap.add_argument("--json", action="store_true", help="emit machine-readable JSON summary")
    ns = ap.parse_args()

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

    try:
        ser = serial.Serial(ns.port, ns.baud, timeout=0.03)
    except Exception as exc:
        print(f"error: open serial failed: {exc}", file=sys.stderr)
        return 2

    try:
        _ = read_for(ser, 1.0)
        for step in steps:
            result = run_step(ser, step, ns.char_delay_ms / 1000.0, ns.retries)
            results.append(result)
            if not result["ok"]:
                break
    finally:
        ser.close()

    ok = all(item["ok"] for item in results) and len(results) == len(steps)
    payload = {
        "ok": ok,
        "time": time.strftime("%Y-%m-%d %H:%M:%S"),
        "port": ns.port,
        "baud": ns.baud,
        "reset": not ns.no_reset,
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
        for item in results:
            status = "PASS" if item["ok"] else "FAIL"
            print(f"[{status}] {item['command']}")
        print(f"log: {log_path}")
        print(f"overall: {'PASS' if ok else 'FAIL'}")

    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
