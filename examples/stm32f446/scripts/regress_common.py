#!/usr/bin/env python3
import os
import subprocess
from contextlib import contextmanager
from pathlib import Path

import fcntl


ROOT = Path(__file__).resolve().parent.parent.parent.parent
OPENOCD_CFG = ROOT / "tools" / "openocd.cfg"
OPENOCD_LOCK_PATH = Path(
    os.environ.get("MK_MPU_OPENOCD_LOCK", "/tmp/microkernel_mpu_openocd.lock")
)
BUILD_SCRIPT = ROOT / "tools" / "build.sh"
FLASH_SCRIPT = ROOT / "tools" / "flash.sh"


@contextmanager
def openocd_lock():
    OPENOCD_LOCK_PATH.parent.mkdir(parents=True, exist_ok=True)
    with OPENOCD_LOCK_PATH.open("a+", encoding="utf-8") as lock_file:
        fcntl.flock(lock_file.fileno(), fcntl.LOCK_EX)
        try:
            yield
        finally:
            fcntl.flock(lock_file.fileno(), fcntl.LOCK_UN)


def run_openocd(command: str, openocd_cfg: Path = OPENOCD_CFG) -> None:
    cmd = ["openocd", "-f", str(openocd_cfg), "-c", command]
    with openocd_lock():
        proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        detail = (proc.stdout + proc.stderr).strip()
        raise RuntimeError(f"openocd command failed (rc={proc.returncode}): {detail}")


def reset_board(openocd_cfg: Path = OPENOCD_CFG) -> None:
    run_openocd("init; reset run; shutdown", openocd_cfg)


def _run_checked(cmd: list[str], action: str, lock_openocd: bool = False) -> None:
    if lock_openocd:
        with openocd_lock():
            proc = subprocess.run(cmd, capture_output=True, text=True, cwd=str(ROOT))
    else:
        proc = subprocess.run(cmd, capture_output=True, text=True, cwd=str(ROOT))

    if proc.returncode != 0:
        detail = (proc.stdout + proc.stderr).strip()
        raise RuntimeError(f"{action} failed (rc={proc.returncode}): {detail}")


def rebuild_and_flash(skip_build: bool = False) -> None:
    if not skip_build:
        if not BUILD_SCRIPT.exists():
            raise RuntimeError(f"build script not found: {BUILD_SCRIPT}")
        _run_checked(["bash", str(BUILD_SCRIPT)], "firmware build")

    if not FLASH_SCRIPT.exists():
        raise RuntimeError(f"flash script not found: {FLASH_SCRIPT}")
    _run_checked(["bash", str(FLASH_SCRIPT)], "firmware flash", lock_openocd=True)
