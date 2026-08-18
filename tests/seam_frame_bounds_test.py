"""End-to-end bounds checks for seam-analyze COBS input."""
import pathlib
import subprocess
import sys


def cobs_encode(data):
    output = bytearray(b"\x00")
    code_index = 0
    code = 1
    for byte in data:
        if byte:
            output.append(byte)
            code += 1
            if code == 0xFF:
                output[code_index] = code
                code_index = len(output)
                output.append(0)
                code = 1
        else:
            output[code_index] = code
            code_index = len(output)
            output.append(0)
            code = 1
    output[code_index] = code
    return bytes(output) + b"\x00"


def run(binary, frame):
    return subprocess.run([binary, "-"], input=frame, capture_output=True,
                          check=False)


def main(binary, fixture):
    valid = cobs_encode(pathlib.Path(fixture).read_bytes())
    for oversized in (b"A" * 9180, b"\x00" * 8200):
        invalid = cobs_encode(oversized)
        if run(binary, invalid).returncode != 1:
            return 1
        recovered = run(binary, invalid + valid)
        if recovered.returncode != 0 or b"VERDICT:" not in recovered.stdout:
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1], sys.argv[2]))
