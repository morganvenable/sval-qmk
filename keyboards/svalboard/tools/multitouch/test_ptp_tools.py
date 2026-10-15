"""Offline tests for the native touchpad dev tools (no board, no HID)."""
import struct
import unittest

import perf
import ptp_hid
from ptp_owner import describe


def owner_reply(status=0, owner=3, mode=3, switches=3, bits=0x65, frames=12, busy=1, scan=4321, pads=3):
    r = bytearray(64)
    r[0] = 0xDD
    r[5], r[6], r[7], r[8], r[9] = 0xFC, 0xC2, status, owner, 3
    s = bytes((1, mode, 0, switches, bits, 2)) + struct.pack("<IIH", frames, busy, scan)
    s += bytes((pads, 0)) + struct.pack("<HHH", 0, 4, 0)
    r[10:34] = s
    r[34:53] = struct.pack("<IIIHHH", 1000, 2, 1, 3, 3, 0) + bytes((5,))
    return bytes(r)


class OwnerReply(unittest.TestCase):
    def test_describe(self):
        text = describe(owner_reply())
        self.assertIn("owner: native", text)
        self.assertIn("input mode: 3 (touchpad)", text)
        self.assertIn("reporting", text)
        self.assertIn("frames sent: 12", text)
        self.assertIn("pads: left, right", text)
        self.assertIn("frames held for the other pad: 4", text)
        self.assertIn("1000 reads", text)
        self.assertIn("hints on", text)

    def test_refused(self):
        self.assertIn("refused, status 2", describe(owner_reply(status=2)))

    def test_one_pad(self):
        self.assertIn("pads: right;", describe(owner_reply(pads=2)))


class PerfReport(unittest.TestCase):
    def half(self, **kw):
        s = {k: 0 for k in perf.PAGE0 + perf.PAGE1 + perf.PAGE2 + perf.PAGE3 + perf.PAGE4}
        s.update(ms=30000, loops=12000, loop_sum=30000000, scans=12000, scan_sum=30000000, i2c_n=3700, i2c_sum=3700 * 1600, pads=3 | 1 << 8, prewait=90, postwait=90)
        s.update(kw)
        s["native"] = bool(s["pads"] >> 8)
        return s

    def test_report(self):
        text = perf.report("USB half", self.half())
        self.assertIn("matrix scans      400/s", text)
        self.assertIn("pad read        123.3/s", text)
        self.assertIn("(contact read)", text)
        self.assertIn("unpaced", text)

    def test_baseline_build(self):
        text = perf.report("USB half", self.half(pads=0))
        self.assertIn("read + end of session", text)
        self.assertNotIn("touchpad", text)

    def test_long_window_refused(self):
        self.assertIn("over 70 minutes", perf.report("USB half", self.half(ms=71 * 60 * 1000)))


class Parser(unittest.TestCase):
    def test_geometry(self):
        g = ptp_hid.geometry(10)
        self.assertEqual((g["gap"], g["xmax"], g["pxmax"]), (476, 4571, 960))
        self.assertEqual(ptp_hid.geometry(0)["xmax"], 4095)

    def test_minimal_descriptor(self):
        # A 1-byte input report: 2 buttons and 6 padding bits.
        desc = bytes.fromhex("05010902a10185010509190129021500250175019502810295068103c0")
        d = ptp_hid.parse(desc)
        self.assertEqual(d.report_bytes("input", 1), 2)
        f, i = d.find("input", 0x090002, 1)[0]
        self.assertEqual(ptp_hid.read_field(bytes((1, 0b10)), f, i), 1)

    def test_rejects_unclosed(self):
        with self.assertRaises(ValueError):
            ptp_hid.parse(bytes.fromhex("05010902a101"))


if __name__ == "__main__":
    unittest.main()
