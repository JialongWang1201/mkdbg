import json
import shutil
import subprocess
import sys
import unittest
from unittest.mock import patch
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "tests" / "fixtures" / "platform"
sys.path.insert(0, str(ROOT / "tools"))
import soc_debug
import firmware_evidence


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



@unittest.skipUnless(shutil.which("tclsh"), "tclsh is required for the OpenOCD simulator")
class SoCTests(unittest.TestCase):
    def run_snapshot(self, fail_wait=False, coordinated=True):
        profile = json.loads((FIXTURE / "soc.json").read_text())
        soc_debug.validate(profile)
        simulation = """
set run(soc.ap) running
set run(soc.mcu) halted
set current soc.mcu
proc target {subcommand} {return $::current}
proc targets {name} {set ::current $name}
proc resume {} {set ::run($::current) running; lappend ::resumed $::current}
proc core {name op args} {
    switch -- $op {
        was_examined {return 1}
        arp_poll {return}
        curstate {return $::run($name)}
        arp_halt {set ::run($name) halted}
        arp_waitstate {
            if {$::fail_wait && $name eq "soc.ap"} {error "halt timeout"}
            if {$::run($name) ne "halted"} {error "core is running"}
        }
        read_memory {
            if {$name eq "soc.mcu" && $::run(soc.ap) ne "halted" && $::coordinated} {
                error "read before coordinated halt"
            }
            return [lrepeat [lindex $args 2] 0x77]
        }
        default {error "unexpected operation $op"}
    }
}
interp alias {} soc.ap {} core soc.ap
interp alias {} soc.mcu {} core soc.mcu
"""
        simulation += f"set fail_wait {int(fail_wait)}\nset coordinated {int(coordinated)}\nset resumed {{}}\n"
        group = profile["halt_groups"]["application-and-mcu"] if coordinated else []
        script = simulation + soc_debug.snapshot_script(profile, group)
        script += '\nputs $mkdbg_output\nputs stderr "$current $run(soc.ap) $run(soc.mcu) $resumed"'
        result = subprocess.run(["tclsh"], input=script, text=True, capture_output=True, check=True)
        restored = "soc.mcu running halted soc.ap" if coordinated else "soc.mcu running halted"
        self.assertEqual(result.stderr.strip(), restored)
        with patch.object(soc_debug, "rpc", return_value=result.stdout.rstrip() + "\n"):
            return soc_debug.snapshot(profile, group, None)

    def test_coordinated_halt_restores_preexisting_halt(self):
        report = self.run_snapshot()
        self.assertEqual(report["reads"]["last-transaction"]["data"], [0x77] * 4)
        self.assertEqual(report["errors"], [])

    def test_failed_wait_still_restores_running_core(self):
        report = self.run_snapshot(fail_wait=True)
        self.assertEqual(report["errors"], ["operation-failed halt timeout"])
        self.assertEqual(report["reads"], {})

    def test_passive_snapshot_preserves_run_state(self):
        report = self.run_snapshot(coordinated=False)
        self.assertEqual(report["reads"]["fabric-error"]["data"], [0x77])
        self.assertEqual(report["errors"], [])



class FirmwareTests(unittest.TestCase):
    def test_retention_expectations_by_reset_type(self):
        records = {
            "hardfault": {"fault": "HardFault", "fault_boot": 7, "boot_count": 8,
                          "valid": 1, "frame_valid": 1, "fault_reboot": 1},
            "deadlock": {"reset": "independent_watchdog", "runtime_valid": 1, "task": "cli"},
            "power-loss": {"reset": "power_on", "runtime_valid": 0, "valid": 0, "boot_count": 1},
        }
        for scenario, record in records.items():
            with self.subTest(scenario=scenario):
                firmware_evidence.verify_scenario(record, scenario)
                with self.assertRaises(ValueError):
                    firmware_evidence.verify_scenario({"valid": 0}, scenario)

    def test_watchdog_checks_previous_runtime_image(self):
        record = {"valid": 1, "build_id": "1" * 40, "runtime_valid": 1,
                  "runtime_build_id": "a" * 40}
        with patch.object(firmware_evidence, "elf_build_id", return_value="a" * 40):
            self.assertEqual(firmware_evidence.verify_build(record, Path("image"), "readelf",
                                                            runtime=True), "a" * 40)
            with self.assertRaisesRegex(ValueError, "build ID mismatch"):
                firmware_evidence.verify_build(record, Path("image"), "readelf")


if __name__ == "__main__":
    unittest.main()
