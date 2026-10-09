import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/'keyboards/svalboard/tools'))
import point_bindings as bindings

class FakeDevice:
    serial = 'test-serial'
    def __init__(self): self.value = 4
    def keymap(self, layer, row, col, value=None):
        if value is not None: self.value = value
        return self.value

class BindingTests(unittest.TestCase):
    def test_round_trip_and_guards(self):
        device=FakeDevice()
        with tempfile.TemporaryDirectory() as directory:
            backup=Path(directory)/'key.json'
            bindings.assign(device,0,1,3,'right-volume-hold',backup)
            self.assertEqual(device.value,0x7E08)
            with self.assertRaises(FileExistsError): bindings.assign(device,0,1,3,'cancel',backup)
            self.assertEqual(device.value,0x7E08)
            device.value=5
            with self.assertRaises(ValueError): bindings.restore(device,backup)
            device.value=0x7E08
            device.serial='another-device'
            with self.assertRaises(ValueError): bindings.restore(device,backup)
            device.serial='test-serial'
            bindings.restore(device,backup)
            self.assertEqual(device.value,4)
