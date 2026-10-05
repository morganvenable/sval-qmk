"""Recovery guarantees for the hardware characterization runner."""
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

TOOLS = Path(__file__).resolve().parents[2] / "keyboards/svalboard/tools"
sys.path.insert(0, str(TOOLS))
import keytest_features as features


class FakeDevice:
    serial = "test-board"
    def __init__(self, serial=None):
        self.active = False
        self.reboots = 0
        self.closed = False
        self.keys = {p: i + 4 for i, p in enumerate(features.POSITIONS)}
        self.tables = {1: bytes(range(10)), 3: bytes(range(12))}
        self.settings = {2: b"\x32\0", 7: b"\xde\0"}
        self.macro = b"Hi!"
    def info(self): return {"active": self.active}
    def begin(self): self.active = True
    def reboot(self): self.active = False; self.reboots += 1
    def close(self): self.closed = True
    def keymap(self, layer, row, col, value=None):
        p = (layer, row, col)
        if value is not None: self.keys[p] = value
        return self.keys[p]
    def exchange(self, request):
        r = bytearray(27)
        r[:2] = request[:2]
        op = request[1]
        if op in (1, 3):
            r[4:4+len(self.tables[op])] = self.tables[op]
        elif op in (2, 4):
            self.tables[op - 1] = bytes(request[4:])
        elif op == 0x11:
            q = int.from_bytes(request[2:4], "little")
            r[3:5] = self.settings[q]
        elif op == 0x12:
            q = int.from_bytes(request[2:4], "little")
            self.settings[q] = bytes(request[4:6])
        elif op == 0x1F:
            r[6] = 3
            r[7:10] = self.macro
        elif op == 0x20:
            self.macro = bytes(request[7:10])
        else:
            raise AssertionError(f"Unexpected opcode {op}")
        return bytes(r)


class FeatureRecoveryTests(unittest.TestCase):
    def test_restores_all_modified_resources_and_reboots(self):
        device = FakeDevice()
        saved = features.snapshot(device)
        device.keys = {p: 100 for p in device.keys}
        device.tables = {1: bytes(10), 3: bytes(12)}
        device.settings = {2: b"\xc8\0", 7: b"\x58\x02"}
        device.macro = b"ab\0"
        features.restore(device, saved)
        self.assertEqual(features.snapshot(device), saved)
        self.assertEqual(device.reboots, 1)
        self.assertFalse(device.active)

    def test_rejects_journal_from_another_board(self):
        device = FakeDevice()
        saved = features.snapshot(device)
        saved["serial"] = "other-board"
        with self.assertRaisesRegex(ValueError, "another board"):
            features.restore(device, saved)
        self.assertEqual(device.reboots, 0)
        self.assertFalse(device.active)

    def test_setup_exception_still_restores_and_records_cleanup(self):
        device = FakeDevice()
        original = features.snapshot(device)
        def fail_setup(dev, output):
            dev.keymap(0, 0, 0, 99)
            raise RuntimeError("simulated setup failure")
        with tempfile.TemporaryDirectory() as directory:
            backup = Path(directory) / "backup.json"
            output = Path(directory) / "result.json"
            argv = ["keytest_features.py", "--serial", device.serial, "--backup", str(backup), "--output", str(output)]
            with patch.object(sys, "argv", argv), patch.object(features, "Device", return_value=device), patch.object(features, "characterize", fail_setup):
                with self.assertRaisesRegex(RuntimeError, "simulated setup failure"):
                    features.main()
            self.assertEqual(json.loads(backup.read_text())["serial"], device.serial)
            result = json.loads(output.read_text())
            self.assertTrue(result["restored"])
            self.assertEqual(result["error"], "simulated setup failure")
        self.assertEqual(features.snapshot(device), original)
        self.assertTrue(device.closed)
