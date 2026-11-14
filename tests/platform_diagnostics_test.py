import json
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "tests" / "fixtures" / "platform"


class BoardTests(unittest.TestCase):
    def test_calibrated_power_reset_and_boot_marker(self):
        result = subprocess.run(
            [sys.executable, str(ROOT / "tools" / "board_evidence.py"),
             str(FIXTURE / "board.json"), "--samples", f"scope={FIXTURE / 'scope.csv'}",
             "--samples", f"logic={FIXTURE / 'logic.csv'}"], capture_output=True, check=True,
        )
        report = json.loads(result.stdout)
        self.assertEqual(report["signals"]["reset-release"]["interval_ns"], [128, 133])
        self.assertEqual(report["signals"]["core-rail"]["status"], "within-window")

    def test_missing_instrument_keeps_evidence_distinct(self):
        result = subprocess.run(
            [sys.executable, str(ROOT / "tools" / "board_evidence.py"),
             str(FIXTURE / "board.json"), "--samples", f"scope={FIXTURE / 'scope.csv'}"],
            capture_output=True,
        )
        self.assertEqual(result.returncode, 1)
        report = json.loads(result.stdout)
        self.assertEqual(report["signals"]["boot-marker"]["status"], "missing")
        self.assertEqual(report["signals"]["core-rail"]["status"], "within-window")


if __name__ == "__main__":
    unittest.main()
