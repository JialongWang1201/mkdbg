"""Configured builds must report missing values without running the command."""

from pathlib import Path
import subprocess
import sys
from tempfile import TemporaryDirectory


def test_build_dry_run(binary):
    cases = (
        (("--port", "--dry-run"), 2, "missing value for --port"),
        (("--target", "--dry-run"), 2, "missing value for --target"),
        (("--port", "/dev/null", "--dry-run"), 0, "touch build-ran"),
        (("--target", "--bench", "--dry-run"), 0, "touch bench-build-ran"),
    )
    with TemporaryDirectory() as directory:
        workdir = Path(directory)
        (workdir / ".mkdbg.toml").write_text(
            'version = 1\n'
            'default_repo = "demo"\n\n'
            '[repos."demo"]\n'
            'preset = "generic"\n'
            'path = "."\n'
            'build_cmd = "touch build-ran"\n\n'
            '[repos."--bench"]\n'
            'preset = "generic"\n'
            'path = "."\n'
            'build_cmd = "touch bench-build-ran"\n',
            encoding="utf-8",
        )
        markers = (workdir / "build-ran", workdir / "bench-build-ran")
        for args, expected_status, expected_message in cases:
            result = subprocess.run(
                [binary, "build", *args],
                cwd=workdir,
                capture_output=True,
                text=True,
                timeout=5,
            )
            for marker in markers:
                assert not marker.exists(), f"build {args}: created {marker.name}"
            assert result.returncode == expected_status, (
                f"build {args}: expected exit {expected_status}, "
                f"got {result.returncode}\n{result.stderr}"
            )
            output = result.stdout if expected_status == 0 else result.stderr
            assert expected_message in output, f"build {args}: {output!r}"


if __name__ == "__main__":
    test_build_dry_run(Path(sys.argv[1]).resolve())
