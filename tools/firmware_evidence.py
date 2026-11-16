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


def verify_build(record, elf, readelf):
    image_id = record.get("build_id")
    require(record["valid"] == 1 and isinstance(image_id, str)
            and re.fullmatch(r"[a-f0-9]{40}", image_id),
            "retained CPU fault lacks a full GNU build ID")
    actual = elf_build_id(elf, readelf)
    require(actual == image_id, f"ELF build ID mismatch: fault={image_id}, elf={actual}")
    return actual


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--elf", type=Path)
    parser.add_argument("--readelf", default="arm-none-eabi-readelf")
    args = parser.parse_args()
    try:
        evidence = load_evidence(args.log)
        report = {"evidence": evidence}
        if args.elf:
            report["verified_build_id"] = verify_build(evidence, args.elf, args.readelf)
        print(json.dumps(report, indent=2, allow_nan=False))
    except subprocess.CalledProcessError as exc:
        parser.exit(2, f"firmware evidence: readelf failed: {exc.stderr.strip()}\n")
    except (OSError, ValueError, KeyError, TypeError, subprocess.TimeoutExpired) as exc:
        parser.exit(2, f"firmware evidence: {exc}\n")


if __name__ == "__main__":
    main()
