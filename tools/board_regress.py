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


def build_steps(include_mpu_overflow: bool) -> list[Step]:
    steps = [
        Step(
            name="io_flood",
            command="vm scenario io_flood",
            timeout_s=5.0,
            must_contain=("scenario result: name=io_flood", "status=halted"),
        ),
        Step(
            name="irq_starvation",
            command="vm scenario irq_starvation",
            timeout_s=4.0,
            must_contain=("scenario result: name=irq_starvation", "status=halted"),
        ),
        Step(
            name="fault_last",
            command="fault last",
            timeout_s=3.0,
            must_contain=("\"type\":\"fault\",\"src\":\"none\"",),
        ),
        Step(
            name="fault_dump",
            command="fault dump",
            timeout_s=4.0,
            must_contain=("No fault recorded", "\"type\":\"fault\",\"src\":\"none\""),
        ),
    ]
    if include_mpu_overflow:
        steps.append(
            Step(
                name="mpu_overflow",
                command="vm scenario mpu_overflow",
                timeout_s=6.0,
                must_contain=("Scenario: mpu_overflow", "\"src\":\"cpu\"", "\"name\":\"MemManage\""),
            )
        )
    return steps


def make_log_path() -> Path:
    LOG_DIR.mkdir(parents=True, exist_ok=True)
    ts = time.strftime("%Y%m%d_%H%M%S")
    return LOG_DIR / f"board_regress_{ts}.log"


def write_log(log_path: Path, payload: dict[str, Any]) -> None:
    lines: list[str] = []
    lines.append(f"time={payload['time']}")
    lines.append(f"port={payload['port']} baud={payload['baud']}")
    lines.append(f"reset={payload['reset']} include_mpu_overflow={payload['include_mpu_overflow']}")
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
        description="Run on-board regression: io_flood, irq_starvation, fault last/dump, mpu_overflow."
    )
    ap.add_argument("--port", required=True, help="serial port, e.g. /dev/cu.usbmodemXXXX")
    ap.add_argument("--baud", type=int, default=115200, help="UART baud rate")
    ap.add_argument("--char-delay-ms", type=float, default=10.0, help="per-char TX delay in ms")
    ap.add_argument("--retries", type=int, default=1, help="retry count per step on mismatch")
    ap.add_argument("--no-reset", action="store_true", help="skip OpenOCD reset before test")
    ap.add_argument("--skip-mpu-overflow", action="store_true", help="skip final crash scenario")
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

    steps = build_steps(include_mpu_overflow=not ns.skip_mpu_overflow)
    results: list[dict[str, Any]] = []

    try:
        ser = serial.Serial(ns.port, ns.baud, timeout=0.03)
    except Exception as exc:
        print(f"error: open serial failed: {exc}", file=sys.stderr)
        return 2

    try:
        _ = read_for(ser, 1.0)
        sync_cli(ser)
        send_line(ser, "disable", ns.char_delay_ms / 1000.0)
        _ = read_for(ser, 0.8)

        for step in steps:
            result = run_step(ser, step, ns.char_delay_ms / 1000.0, ns.retries)
            results.append(result)
    finally:
        ser.close()

    ok = all(item["ok"] for item in results)
    payload = {
        "ok": ok,
        "time": time.strftime("%Y-%m-%d %H:%M:%S"),
        "port": ns.port,
        "baud": ns.baud,
        "reset": not ns.no_reset,
        "include_mpu_overflow": not ns.skip_mpu_overflow,
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
