"""Malformed interactive numbers must not produce target requests."""

import os
import pty
import subprocess
import sys

from debug_run_control_test import Target, packet


def test_invalid_numbers(binary):
    invalid = []
    for command in ("break", "watch", "rwatch", "awatch", "mem", "display"):
        for address in ("0x100000000", "0x08001000junk", "-1", "+1"):
            invalid.append(f"{command} {address}")
    master, slave = pty.openpty()
    process = None
    try:
        process = subprocess.Popen(
            [binary, "debug", "--port", os.ttyname(slave)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
        process.stdin.write(("\n".join(invalid + ["mem 0x20000000 1", "quit"]) + "\n").encode())
        process.stdin.flush()
        target = Target(master)
        request = target.event()
        assert request == b"m20000000,1", ("invalid input reached target", request)
        os.write(master, b"+" + packet(b"aa"))
        assert target.event() == b"c"
        stdout, stderr = process.communicate(timeout=5)
        assert process.returncode == 0, (stdout, stderr)
        assert stdout.count(b"error:") == len(invalid), stdout
        assert b"0x20000000: aa" in stdout, stdout
    finally:
        if process is not None:
            if process.poll() is None:
                process.kill()
            process.communicate()
        os.close(master)
        os.close(slave)


if __name__ == "__main__":
    test_invalid_numbers(sys.argv[1])
    print("debug_numeric_input_test: OK")
