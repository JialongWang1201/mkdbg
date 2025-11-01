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
        Step("show_initial", "kdi driver show", 1.1, ("kdi state drv=kernel st=active", "kdi state drv=uart st=active")),
        Step("runtime_error", "kdi driver error uart 85", 1.0, ("kdi driver error uart: ok",)),
        Step("show_error", "kdi driver show", 1.1, ("kdi state drv=uart st=error",)),
        Step("reset_from_error", "kdi driver reset uart", 1.0, ("kdi driver reset uart: ok",)),
        Step("show_reset", "kdi driver show", 1.1, ("kdi state drv=uart st=reset",)),
        Step("reinit_from_reset", "kdi driver reinit uart", 1.0, ("kdi driver reinit uart: ok",)),
        Step("show_init", "kdi driver show", 1.1, ("kdi state drv=uart st=init", "tok_active=1")),
        Step("init_blocks_runtime", "kdi probe allow", 1.0, ("kdi probe allow: state",)),
        Step("probe_start", "kdi driver probe uart", 1.0, ("kdi driver probe uart: ok",)),
        Step("probe_fail", "kdi driver fail uart", 1.0, ("kdi driver fail uart: ok",)),
        Step("show_probe_fail_error", "kdi driver show", 1.1, ("kdi state drv=uart st=error",)),
        Step("reset_after_probe_fail", "kdi driver reset uart", 1.0, ("kdi driver reset uart: ok",)),
        Step("reinit_after_probe_fail", "kdi driver reinit uart", 1.0, ("kdi driver reinit uart: ok",)),
        Step("probe_recover", "kdi driver probe uart", 1.0, ("kdi driver probe uart: ok",)),
        Step("ready_recover", "kdi driver ready uart", 1.0, ("kdi driver ready uart: ok",)),
        Step("active_recover", "kdi driver active uart", 1.0, ("kdi driver active uart: ok",)),
        Step("show_recovered", "kdi driver show", 1.1, ("kdi state drv=uart st=active",)),
        Step("runtime_ok", "kdi probe allow", 1.0, ("kdi probe allow: ok",)),
        Step("kernel_reclaim_denied", "kdi driver reclaim kernel", 1.0, ("kdi driver reclaim kernel: denied",)),
        Step("reclaim_uart", "kdi driver reclaim uart", 1.0, ("kdi driver reclaim uart: ok",)),
        Step("show_dead", "kdi driver show", 1.1, ("kdi state drv=uart st=dead", "tok_active=0")),
        Step("dead_probe_auth", "kdi driver probe uart", 1.0, ("kdi driver probe uart: auth",)),
        Step("reinit_dead", "kdi driver reinit uart", 1.0, ("kdi driver reinit uart: ok",)),
        Step("probe_final", "kdi driver probe uart", 1.0, ("kdi driver probe uart: ok",)),
        Step("ready_final", "kdi driver ready uart", 1.0, ("kdi driver ready uart: ok",)),
        Step("active_final", "kdi driver active uart", 1.0, ("kdi driver active uart: ok",)),
        Step("runtime_final", "kdi probe allow", 1.0, ("kdi probe allow: ok",)),
        Step("stats_final", "kdi show", 1.2, ("st_fail=", "reclaim=")),
        Step("cap_show_final", "kdi cap show", 1.2, ("kdi cap drv=uart", "decl_unused=", "active=")),
        Step("cap_json_final", "kdi cap json", 1.2, ('{"type":"kdi-cap","driver":"uart"', '"declared_not_used"')),
        Step("cap_review_final", "kdi cap review uart", 1.2, ("kdi cap review drv=uart", "window=obs[", "risk=")),
        Step(
            "cap_review_json_final",
            "kdi cap review-json uart",
            1.2,
            ('{"type":"kdi-cap-review","driver":"uart"', '"review":"policy.allow_'),
        ),
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
    return LOG_DIR / f"kdi_driver_regress_{ts}.log"


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
        description="Run on-board KDI driver lifecycle regression workflow."
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
