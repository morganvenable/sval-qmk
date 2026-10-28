"""The Python codec (tools/board_id/board_record.py) against the C one.

    test_codec.py VECTOR.bin    a record test_board_record.c built
"""
import os
import sys
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "keyboards", "svalboard", "tools", "board_id"))
import board_record as br  # noqa: E402

VECTOR = None
UID = bytes.fromhex("E46450D2A7094232")
FIELDS = br.tlv([(br.TAG_MODEL, "SvalMCUMini"), (br.TAG_REVISION, "v1.0b0"), (br.TAG_LOT, "LOT1")])


class Codec(unittest.TestCase):
    def test_same_bytes_as_c(self):
        with open(VECTOR, "rb") as f:
            c_page = f.read()
        py = br.build(UID, 1761696000, FIELDS, strap=[br.STRAP_FLOATING, 64, 0, 17])
        self.assertEqual(c_page[:len(py)], py)
        self.assertEqual(c_page[len(py):], b"\xff" * (br.PAGE - len(py)))
        rec = br.check(c_page)
        self.assertEqual((rec["model"], rec["revision"], rec["lot"], rec["uid"]), ("SvalMCUMini", "v1.0b0", "LOT1", UID.hex().upper()))
        self.assertEqual((rec["strap"], rec["strap_counts"]), ("floating", [64, 0, 17]))

    def test_scan_history(self):
        block = bytearray(b"\xff" * br.SIZE)
        a = br.build(UID, 1, br.station_tlv("SvalMCUMini", "v1.0b0", "A", "s"))
        b = br.build(UID, 2, br.station_tlv("SvalMCUMini", "v1.0b0", "B", "s"))
        block[0:len(a)] = a
        block[256:256 + 10] = b[:10]  # torn
        block[512:512 + len(b)] = b
        s = br.scan(bytes(block))
        self.assertEqual((s["identities"], s["newest"], s["damaged"], s["used"], s["append"]), (2, 2, 1, 3, 3))
        self.assertEqual(s["current"]["lot"], "B")

    def test_params_and_report_sizes(self):
        p = br.params(1, 2, FIELDS, br.STRAP_LOW, br.FLAG_DRY)
        self.assertEqual(len(p), br.PARAMS_BYTES)
        self.assertIsNone(br.parse_report(b"\0" * 40))
        with self.assertRaises(ValueError):
            br.params(1, 2, b"\x07\xff" + b"x" * 255)
        with self.assertRaises(ValueError):
            br.build(UID, 0, br.tlv([(br.TAG_NOTES, "x" * 240)]))


if __name__ == "__main__":
    VECTOR = sys.argv.pop(1)
    unittest.main(verbosity=1)
