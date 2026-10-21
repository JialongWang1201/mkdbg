"""Exercise the CLI's real SIGINT path over a PTY without target hardware."""

import os
import pty
import select
import signal
import subprocess
import sys
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


if __name__ == "__main__":
    for confirmation in (False, True):
        test_interrupt(sys.argv[1], confirmation)
    print("debug_run_control_test: OK")
