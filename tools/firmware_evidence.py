#!/usr/bin/env python3
"""Inspect a post-reset firmware evidence record and verify its image identity."""

import argparse
import json
import os
import re
import subprocess
from pathlib import Path

from board_evidence import require


def load_evidence(path):
    evidence = None
    with path.open("rb") as stream:
        while line := stream.readline(65537):
            require(len(line) <= 65536, f"{path}: oversized capture line")
            if not line.lstrip().startswith(b'{') or b'"firmware-evidence"' not in line:
                continue
            record = json.loads(line)
            if record.get("type") != "firmware-evidence":
                continue
            require(evidence is None or evidence == record, f"{path}: ambiguous evidence records")
            evidence = record
    require(evidence is not None, f"{path}: firmware evidence record missing")
    require(type(evidence.get("version")) is int and evidence["version"] == 1,
            "unsupported firmware evidence version")
    require(type(evidence.get("valid")) is int and evidence["valid"] in (0, 1),
            "invalid fault validity flag")
    return evidence


def elf_build_id(path, readelf):
    executable = path.resolve(strict=True)
    env = {**os.environ, "LC_ALL": "C"}
    output = subprocess.run([readelf, "-n", "--", str(executable)], text=True,
                            capture_output=True, check=True, timeout=15, env=env)
    identifiers = re.findall(r"^\s*Build ID: ([a-fA-F0-9]+)\s*$", output.stdout, re.M)
    require(len(identifiers) == 1, f"{executable}: expected exactly one ELF build ID")
    return identifiers[0].lower()


def verify_build(record, elf, readelf, runtime=False):
    key = "runtime_build_id" if runtime else "build_id"
    valid = record.get("runtime_valid", 0) if runtime else record["valid"]
    image_id = record.get(key)
    require(valid == 1 and isinstance(image_id, str) and re.fullmatch(r"[a-f0-9]{40}", image_id),
            f"retained record lacks a full {key}")
    actual = elf_build_id(elf, readelf)
    require(actual == image_id, f"ELF build ID mismatch: retained={image_id}, elf={actual}")
    return actual


def verify_scenario(record, scenario):
    if scenario == "hardfault":
        require(record.get("fault") == "HardFault" and record["valid"] == 1
                and record.get("frame_valid") == 1 and record.get("fault_reboot") == 1
                and record["fault_boot"] + 1 == record["boot_count"],
                "expected a valid HardFault frame from the preceding boot")
    elif scenario == "deadlock":
        require(record.get("reset") in ("independent_watchdog", "window_watchdog")
                and record.get("runtime_valid") == 1 and record.get("task"),
                "expected watchdog reset with previous-boot task context")
    else:
        require(record.get("reset") in ("power_on", "brown_out") and record["valid"] == 0
                and record.get("runtime_valid") == 0 and record.get("boot_count") == 1,
                "expected fresh boot with no pre-power-cycle SRAM evidence")


SYSTEM_EXCEPTIONS = {
    2: "NMI", 3: "HardFault", 4: "MemManage", 5: "BusFault", 6: "UsageFault",
    11: "SVCall", 12: "DebugMonitor", 14: "PendSV", 15: "SysTick",
}


def decode_exception(number, no_exception):
    require(type(number) is int and 0 <= number <= 511, "invalid Cortex-M exception number")
    if number == 0:
        return {"kind": no_exception}
    if number < 16:
        return {"kind": "system", "number": number,
                "name": SYSTEM_EXCEPTIONS.get(number, "reserved")}
    return {"kind": "external-irq", "exception": number, "irq": number - 16}


def symbolize(record, elf, addr2line):
    if record.get("frame_valid") != 1:
        return None
    addresses = []
    for name in ("pc", "lr"):
        address = record[name]
        require(isinstance(address, str) and re.fullmatch(r"0x[0-9a-fA-F]{8}", address),
                f"invalid retained {name}")
        addresses.append(f"0x{int(address, 16) & ~1:x}")
    executable = str(elf.resolve(strict=True))
    result = subprocess.run([addr2line, "-f", "-e", executable, *addresses], check=True,
                            text=True, capture_output=True, timeout=15)
    lines = result.stdout.splitlines()
    require(len(lines) == 4, "unexpected addr2line reply")
    return {"pc": {"function": lines[0], "location": lines[1]},
            "lr": {"function": lines[2], "location": lines[3]}}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--elf", type=Path)
    parser.add_argument("--readelf", default="arm-none-eabi-readelf")
    parser.add_argument("--addr2line", default="arm-none-eabi-addr2line")
    parser.add_argument("--expect", choices=("hardfault", "deadlock", "power-loss"))
    args = parser.parse_args()
    if args.expect in ("hardfault", "deadlock") and not args.elf:
        parser.error("--expect hardfault/deadlock requires --elf")
    try:
        evidence = load_evidence(args.log)
        report = {"evidence": evidence}
        if "irq_exception" in evidence:
            report["previous_irq"] = decode_exception(evidence["irq_exception"], "unobserved")
        if evidence.get("frame_valid") == 1 and "fault_exception" in evidence:
            report["fault_context"] = decode_exception(evidence["fault_exception"], "thread")
        if args.expect:
            verify_scenario(evidence, args.expect)
            report["scenario"] = args.expect
        if args.elf:
            runtime = args.expect == "deadlock" or evidence["valid"] == 0
            report["verified_build_id"] = verify_build(evidence, args.elf, args.readelf, runtime)
            if not runtime:
                report["symbols"] = symbolize(evidence, args.elf, args.addr2line)
        print(json.dumps(report, indent=2, allow_nan=False))
    except subprocess.CalledProcessError as exc:
        parser.exit(2, f"firmware evidence: ELF tool failed: {exc.stderr.strip()}\n")
    except (OSError, ValueError, KeyError, TypeError, subprocess.TimeoutExpired) as exc:
        parser.exit(2, f"firmware evidence: {exc}\n")


if __name__ == "__main__":
    main()
