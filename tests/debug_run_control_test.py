"""Exercise the CLI's real SIGINT path over a PTY without target hardware."""

import os
from pathlib import Path
import pty
import select
import signal
import subprocess
import sys
import tempfile
import time


def packet(payload):
    return b"$" + payload + b"#" + f"{sum(payload) & 255:02x}".encode()


class Target:
    def __init__(self, fd):
        self.fd = fd
        self.pending = bytearray()

    def event(self, timeout=5):
        deadline = time.monotonic() + timeout
        while True:
            while self.pending:
                first = self.pending[0]
                if first == 3:
                    del self.pending[0]
                    return b"\x03"
                if first != ord("$"):
                    del self.pending[0]  # Response ACKs.
                    continue
                end = self.pending.find(b"#")
                if end < 0 or len(self.pending) < end + 3:
                    break
                frame = bytes(self.pending[:end + 3])
                del self.pending[:end + 3]
                assert frame == packet(frame[1:-3]), frame
                return frame[1:-3]
            remaining = deadline - time.monotonic()
            if remaining <= 0 or not select.select([self.fd], [], [], remaining)[0]:
                raise AssertionError("timed out waiting for debugger request")
            self.pending.extend(os.read(self.fd, 4096))


def test_interrupt(binary, running_confirmation):
    master, slave = pty.openpty()
    process = None
    try:
        process = subprocess.Popen(
            [binary, "debug", "--port", os.ttyname(slave)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
        process.stdin.write(b"continue\nquit\n")
        process.stdin.flush()
        target = Target(master)
        assert target.event() == b"c"
        os.write(master, b"+")
        if running_confirmation:
            os.write(master, packet(b"S00"))

        # Exceed the ordinary 2 s transaction timeout before interrupting.
        time.sleep(2.2)
        assert process.poll() is None, "continue returned before a real halt"
        process.send_signal(signal.SIGINT)
        assert target.event() == b"\x03", "SIGINT did not reach the transport"
        os.write(master, packet(b"S02"))
        assert target.event() == b"g"
        os.write(master, b"+" + packet(b"00000000" * 17))
        assert target.event() == b"c"  # quit resumes the target.
        stdout, stderr = process.communicate(timeout=5)
        assert process.returncode == 0, (stdout, stderr)
        assert b"Halted.  signal=2" in stdout, stdout
        assert b"continue failed" not in stdout, stdout
    finally:
        if process is not None:
            if process.poll() is None:
                process.kill()
            process.communicate()
        os.close(master)
        os.close(slave)


def test_spontaneous_stop_before_regs(binary):
    master, slave = pty.openpty()
    process = None
    try:
        process = subprocess.Popen(
            [binary, "debug", "--port", os.ttyname(slave)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
        # Wait for raw serial configuration before queuing an unsolicited stop.
        initial_output = bytearray()
        deadline = time.monotonic() + 5
        while b"(mkdbg) " not in initial_output:
            remaining = deadline - time.monotonic()
            assert remaining > 0 and select.select([process.stdout], [], [], remaining)[0]
            chunk = os.read(process.stdout.fileno(), 4096)
            assert chunk, "debugger exited before the initial prompt"
            initial_output.extend(chunk)

        target = Target(master)
        os.write(master, packet(b"S05"))
        process.stdin.write(b"regs\nmem 0x20000000 4\nquit\n")
        process.stdin.flush()
        assert target.event() == b"g"
        os.write(master, b"+" + packet(b"00000000" * 17))
        assert target.event() == b"m20000000,4", "stop notification caused a command retransmission"
        os.write(master, b"+" + packet(b"01020304"))
        assert target.event() == b"c"
        stdout, stderr = process.communicate(timeout=5)
        assert process.returncode == 0, (stdout, stderr)
        assert b"r0" in stdout and b"01 02 03 04" in stdout, stdout
        assert b"error:" not in stdout, stdout
    finally:
        if process is not None:
            if process.poll() is None:
                process.kill()
            process.communicate()
        os.close(master)
        os.close(slave)


def test_debug_target(binary):
    with tempfile.TemporaryDirectory(prefix="mkdbg-debug-target-") as tmp:
        root = Path(tmp).resolve()
        board = root / "boards" / "pico"
        board.mkdir(parents=True)
        nested = root / "nested"
        nested.mkdir()
        elf = board / "build" / "swift-blinky.elf"

        def run(*args, cwd=root, expected=0):
            result = subprocess.run([binary, *args], cwd=cwd, text=True,
                                    capture_output=True, timeout=5)
            assert result.returncode == expected, (args, result.stdout, result.stderr)
            return result.stdout + result.stderr

        run("init", "--preset", "generic", "--name", "default-board")
        run("target", "add", "pico", "--path", "boards/pico",
            "--elf-path", "build/swift-blinky.elf", "--openocd-cfg", "openocd.cfg",
            "--chip", "RP2040", "--arch", "cortex-m", "--port", "/dev/ttyPico")
        run("target", "add", "legacy", "--path", ".")

        flash = run("probe", "flash", "--target", "pico", "--dry-run", cwd=nested)
        assert str(elf) in flash, flash
        debug = run("debug", "--target", "pico", "--probe", "--dry-run", cwd=nested)
        assert "probe_open(auto, RP2040)" in debug and "arch=cortex-m" in debug, debug
        assert f"elf={elf}" in debug, debug
        serial = run("debug", "--target", "pico", "--dry-run", cwd=nested)
        assert "uart_open(/dev/ttyPico, 115200)" in serial and f"elf={elf}" in serial, serial
        replay = run("debug", "--target", "pico", "--replay-debug", "session.cap", "--dry-run")
        assert "replay debug capture=session.cap arch=cortex-m" in replay and f"elf={elf}" in replay, replay

        override = run("debug", "--target", "pico", "--probe", "0",
                       "--chip", "Override", "--arch", "riscv32", "--elf", "override.elf", "--dry-run")
        assert "probe_open(selected, Override)" in override and "arch=riscv32" in override, override
        assert "elf=override.elf" in override and str(elf) not in override, override

        legacy = run("debug", "--target", "legacy", "--probe", "--dry-run")
        assert "probe_open(auto, auto)" in legacy and "arch=cortex-m" in legacy, legacy
        assert "elf=" not in legacy, legacy
        missing = run("debug", "--target", "unknown", "--probe", "--dry-run", expected=2)
        assert "repo `unknown` not found" in missing, missing


if __name__ == "__main__":
    for confirmation in (False, True):
        test_interrupt(sys.argv[1], confirmation)
    test_spontaneous_stop_before_regs(sys.argv[1])
    test_debug_target(os.path.abspath(sys.argv[1]))
    print("debug_run_control_test: OK")
