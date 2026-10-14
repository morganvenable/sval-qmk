"""Offline tests for the native touchpad host tools (no board, no HID)."""
import struct
import unittest

import ptp_hid
from ptp_owner import describe, not_built


def reply(status=0, owner=3, mode=3, switches=3, bits=0x65, frames=12, busy=1, scan=4321):
    r = bytearray(64)
    r[0] = 0xDD
    r[5], r[6], r[7], r[8], r[9] = 0xFC, 0xC2, status, owner, 1
    s = bytes((1, mode, 0, switches, bits, 2)) + struct.pack("<IIH", frames, busy, scan)
    r[10:26] = s
    return bytes(r)


class OwnerReply(unittest.TestCase):
    def test_describe(self):
        text = describe(reply())
        self.assertIn("owner: native", text)
        self.assertIn("input mode: 3 (touchpad)", text)
        self.assertIn("reporting", text)
        self.assertIn("frames sent: 12", text)

    def test_refused(self):
        self.assertIn("refused, status 2", describe(reply(status=2)))

    def test_old_firmware(self):
        r = bytearray(64)
        r[5], r[6], r[7], r[8] = 0xFC, 0xC2, 2, 1
        r[12:14] = (2048).to_bytes(2, "little")
        self.assertTrue(not_built(bytes(r)))
        self.assertFalse(not_built(reply(status=2)))


class Parser(unittest.TestCase):
    def test_geometry(self):
        g = ptp_hid.geometry(10)
        self.assertEqual((g["gap"], g["xmax"], g["pxmax"]), (476, 4571, 960))
        self.assertEqual(ptp_hid.geometry(0)["xmax"], 4095)

    def test_minimal_descriptor(self):
        # A 1-byte input report: 2 buttons and 6 padding bits.
        desc = bytes.fromhex("05010902a101850105091901290215002501750195028102950681 03c0".replace(" ", ""))
        d = ptp_hid.parse(desc)
        self.assertEqual(d.report_bytes("input", 1), 2)
        f, i = d.find("input", 0x090002, 1)[0]
        self.assertEqual(ptp_hid.read_field(bytes((1, 0b10)), f, i), 1)

    def test_rejects_unclosed(self):
        with self.assertRaises(ValueError):
            ptp_hid.parse(bytes.fromhex("05010902a101"))


if __name__ == "__main__":
    unittest.main()
