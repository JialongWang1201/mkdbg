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
    return [
        Step("disable_logs", "disable", 0.9, ("status:",)),
        Step("irq_show_initial", "kdi irq show", 1.2, ("kdi irq stats", "kdi irq drv=uart")),
        Step("irq_budget_uart", "kdi irq budget uart 5", 1.0, ("kdi irq budget uart=5 rc=ok",)),
        Step("irq_cooldown_uart", "kdi irq cooldown uart 5000 5000", 1.0, ("kdi irq cooldown uart base=5000 max=5000 rc=ok",)),
        Step("irq_starve_1ms", "kdi irq starve 1", 1.0, ("kdi irq starve=1 rc=ok",)),
        Step("irq_storm_uart", "kdi irq storm uart 12", 1.2, ("kdi irq storm uart", "fail=")),
        Step("irq_show_cooldown_active", "kdi irq show", 1.2, ("kdi irq drv=uart in_irq=0 thr=1",)),
        Step("irq_poll_starve", "kdi irq poll", 1.0, ("kdi irq poll detected=1 rc=ok",)),
        Step("irq_worker_4", "kdi irq worker 4", 1.0, ("kdi irq worker n=4 processed=", "rc=ok")),
        Step("irq_budget_unlimited", "kdi irq budget uart 0", 1.0, ("kdi irq budget uart=0 rc=ok",)),
        Step("irq_enter_uart", "kdi irq enter uart", 1.0, ("kdi irq enter uart: ok",)),
        Step("irq_defer_uart", "kdi irq defer uart 33 7", 1.0, ("kdi irq defer uart work=0x0021", "rc=ok")),
        Step("irq_exit_uart", "kdi irq exit uart", 1.0, ("kdi irq exit uart: ok",)),
        Step("irq_enter_sensor", "kdi irq enter sensor", 1.0, ("kdi irq enter sensor: ok",)),
        Step("irq_unsafe_malloc", "kdi irq unsafe sensor malloc", 1.0, ("kdi irq unsafe sensor op=malloc rc=state",)),
        Step("irq_unsafe_printf", "kdi irq unsafe sensor printf", 1.0, ("kdi irq unsafe sensor op=printf rc=state",)),
        Step("irq_unsafe_policy", "kdi irq unsafe sensor policy", 1.0, ("kdi irq unsafe sensor op=policy rc=state",)),
        Step("irq_defer_sensor", "kdi irq defer sensor 34 9", 1.0, ("kdi irq defer sensor work=0x0022", "rc=ok")),
        Step("irq_exit_sensor", "kdi irq exit sensor", 1.0, ("kdi irq exit sensor: ok",)),
        Step("irq_worker_flush", "kdi irq worker 16", 1.0, ("kdi irq worker n=16 processed=", "rc=ok")),
        Step("kdi_show_irq_stats", "kdi show", 1.2, ("kdi irq enter=", "unsafe=")),
    ]


def build_preflight_steps() -> list[Step]:
    return [
        Step("preflight_fault_show", "kdi fault show", 1.2, ("kdi fault drv=kernel", "contain=")),
        Step("preflight_fault_domain_uart", "kdi fault domain uart", 1.0, ("kdi fault drv=uart",)),
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
        _ = read_for(ser, 1.0)
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
    return LOG_DIR / f"kdi_irq_regress_{ts}.log"


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
        description="Run on-board KDI IRQ/deferred-concurrency regression workflow."
    )
    ap.add_argument("--port", required=True, help="serial port, e.g. /dev/cu.usbmodemXXXX")
    ap.add_argument("--baud", type=int, default=115200, help="UART baud rate")
    ap.add_argument("--char-delay-ms", type=float, default=10.0, help="per-char TX delay in ms")
    ap.add_argument("--retries", type=int, default=2, help="retry count per step on mismatch")
    ap.add_argument("--no-reset", action="store_true", help="skip OpenOCD reset before test")
    ap.add_argument("--flash", action="store_true", help="build and flash firmware before test")
    ap.add_argument("--flash-skip-build", action="store_true", help="with --flash, skip build and only flash existing ELF")
    ap.add_argument(
        "--no-auto-flash-stale",
        action="store_true",
        help="do not auto flash when KDI preflight detects stale firmware",
    )
    ap.add_argument("--json", action="store_true", help="emit machine-readable JSON summary")
    ns = ap.parse_args()

    if not OPENOCD_CFG.exists():
        print(f"error: OpenOCD config not found: {OPENOCD_CFG}", file=sys.stderr)
        return 2

    steps = build_steps()
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
            "warning: KDI preflight failed; rebuilding and flashing firmware once",
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
        command = bad["command"] if bad is not None else "kdi fault show"
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
        for item in results:
            status = "PASS" if item["ok"] else "FAIL"
            print(f"[{status}] {item['command']}")
        print(f"log: {log_path}")
        print(f"overall: {'PASS' if ok else 'FAIL'}")

    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
