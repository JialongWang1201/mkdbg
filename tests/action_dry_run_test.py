"""Configured builds must report missing values without running the command."""

from pathlib import Path
import subprocess
import sys
from tempfile import TemporaryDirectory


def test_build_dry_run(binary):
    cases = (
        (("--port", "--dry-run"), 2, "missing value for --port"),
        (("--target", "--dry-run"), 2, "missing value for --target"),
        (("--port", "/dev/null", "--dry-run"), 0, None),
    )
    with TemporaryDirectory() as directory:
        workdir = Path(directory)
        (workdir / ".mkdbg.toml").write_text(
            'version = 1\n'
            'default_repo = "demo"\n\n'
            '[repos."demo"]\n'
            'preset = "generic"\n'
            'path = "."\n'
            'build_cmd = "touch build-ran"\n',
            encoding="utf-8",
        )
        marker = workdir / "build-ran"
        for args, expected_status, diagnostic in cases:
            result = subprocess.run(
                [binary, "build", *args],
                cwd=workdir,
                capture_output=True,
                text=True,
                timeout=5,
            )
            assert not marker.exists(), f"build {args}: configured command ran"
            assert result.returncode == expected_status, (
                f"build {args}: expected exit {expected_status}, "
                f"got {result.returncode}\n{result.stderr}"
            )
            if diagnostic is not None:
                assert diagnostic in result.stderr, f"build {args}: {result.stderr!r}"


if __name__ == "__main__":
    test_build_dry_run(Path(sys.argv[1]).resolve())
