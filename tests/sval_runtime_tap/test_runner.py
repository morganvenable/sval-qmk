"""Recovery checks for the hardware runner, without accessing a device."""
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'keyboards/svalboard/tools'))
import keytest_runtime_tap as runner


class Board:
    serial = 'mule-test'

    def __init__(self):
        self.keys = {p: 4 + i for i, p in enumerate(runner.POSITIONS)}
        self.settings = {q: bytes([q, 0]) for q in runner.QSIDS}
        self.macro = bytearray(range(16))
        self.active = False
        self.closed = False
        self.reboots = 0

    def info(self):
        return dict(active=self.active)

    def begin(self):
        self.active = True

    def close(self):
        self.closed = True

    def reboot(self):
        self.active = False
        self.reboots += 1

    def keymap(self, layer, row, col, code=None):
        pos = (layer, row, col)
        if code is not None:
            self.keys[pos] = code
        return self.keys[pos]

    def exchange(self, request):
        r = bytearray(27)
        r[:2] = request[:2]
        op = request[1]
        if op in (0x11, 0x12):
            q = int.from_bytes(request[2:4], 'little')
            if op == 0x11:
                r[3:5] = self.settings[q]
            else:
                self.settings[q] = bytes(request[4:6])
        elif op == 0x1F:
            r[6] = 16
            r[7:23] = self.macro
        elif op == 0x20:
            count = request[6]
            self.macro[:count] = request[7:7 + count]
        return bytes(r)


class Recovery(unittest.TestCase):
    def test_transport_failure_still_restores_and_reboots(self):
        board = Board()
        original = runner.snapshot(board)
        with tempfile.TemporaryDirectory() as directory:
            backup, output = (Path(directory) / name for name in ('backup.json', 'output.json'))

            def fail(d, out):
                self.assertEqual(json.loads(backup.read_text()), original)
                d.keymap(*runner.POSITIONS[0], 0x7700)
                runner.setting_set(d, 18, b'\x2c\x01')
                runner.macro(d, b'a\0')
                raise RuntimeError('simulated transport failure')

            with patch.object(runner, 'Device', return_value=board), patch.object(runner, 'run', side_effect=fail), patch.object(sys, 'argv', ['keytest', '--serial', board.serial, '--backup', str(backup), '--output', str(output)]):
                with self.assertRaisesRegex(RuntimeError, 'simulated transport'):
                    runner.main()
            self.assertEqual(runner.snapshot(board), original)
            self.assertEqual(board.reboots, 1)
            self.assertTrue(board.closed)
            self.assertTrue(json.loads(output.read_text())['restored'])

    def test_existing_journal_is_never_overwritten(self):
        board = Board()
        with tempfile.TemporaryDirectory() as directory:
            backup = Path(directory) / 'backup.json'
            backup.write_text('existing recovery journal')
            with patch.object(runner, 'Device', return_value=board), patch.object(runner, 'run') as run, patch.object(sys, 'argv', ['keytest', '--serial', board.serial, '--backup', str(backup), '--output', str(Path(directory) / 'out.json')]):
                with self.assertRaises(FileExistsError):
                    runner.main()
                run.assert_not_called()
            self.assertEqual(backup.read_text(), 'existing recovery journal')
            self.assertFalse(board.active)
