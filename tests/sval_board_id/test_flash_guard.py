"""The board record block stays untouched: the flash map, the update tools, real builds.

    test_flash_guard.py [REAL.uf2 ...]    optional QMK builds to check as well

The C side (static asserts, slot_flash_op refusing the block) is in
board_record_sval.c, updater/update_flash.c and tests/sval_updater/test_updater.c.
"""
import glob
import os
import re
import sys
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
KB = os.path.join(ROOT, "keyboards", "svalboard")
sys.path.insert(0, os.path.join(KB, "tools"))
sys.path.insert(0, os.path.join(KB, "tools", "board_id"))
import board_record as br  # noqa: E402
import make_update  # noqa: E402
import uf2  # noqa: E402

REAL = []


def define(rel, name):
    with open(os.path.join(KB, rel), encoding="utf-8") as f:
        text = f.read()
    m = re.search(rf"#\s*define\s+{name}\s+\(?([^/\n]+?)\)?\s*(//.*)?$", text, re.M)
    if not m:
        raise AssertionError(f"{name} not in {rel}")
    return int(eval(m.group(1).replace("u", "").replace("U", ""), {}, {}))  # numbers and * only


class FlashMap(unittest.TestCase):
    def regions(self):
        wl_base = define("config.h", "WEAR_LEVELING_RP2040_FLASH_BASE")
        wl_end = wl_base + define("config.h", "WEAR_LEVELING_BACKING_SIZE") * define("config.h", "WEAR_LEVELING_COPIES")
        slot = define("config.h", "SVAL_UPDATE_BASE")
        with open(os.path.join(KB, "flash_reservation.ld"), encoding="utf-8") as f:
            fw_end = int(re.search(r"__flash_binary_end <= 0x([0-9A-Fa-f]+)", f.read()).group(1), 16) - 0x10000000
        pair = define("updater/update_pair.h", "UPDATE_PAIR_RECORD_OFFSET")
        return {
            "firmware": (0, fw_end),
            "update image (commit range)": (0, define("config.h", "SVAL_UPDATE_MAX_IMAGE")),
            "settings": (wl_base, wl_end),
            "pair record": (pair, pair + 0x1000),
            "staging slot": (slot, slot + define("config.h", "SVAL_UPDATE_SIZE")),
            "side records": (define("side.h", "SVAL_SIDE_SECTOR_A"), define("side.h", "SVAL_SIDE_SECTOR_B") + 0x1000),
            "build reset": (define("build_reset.h", "BUILD_RESET_SECTOR"), define("build_reset.h", "BUILD_RESET_SECTOR") + 0x1000),
            "identity": (define("identity.h", "IDENTITY_SECTOR_A"), define("identity.h", "IDENTITY_SECTOR_B") + 0x1000),
        }

    def test_header_matches_python(self):
        self.assertEqual(define("board_record.h", "BOARD_RECORD_BASE"), br.BASE)
        self.assertEqual(define("board_record.h", "BOARD_RECORD_SIZE"), br.SIZE)
        self.assertEqual(make_update.BOARD_RECORD, (br.BASE, br.BASE + br.SIZE))
        self.assertEqual(make_update.PROTECTED["board record"], make_update.BOARD_RECORD)

    def test_no_region_shares_its_block(self):
        lo, hi = br.BASE, br.BASE + br.SIZE
        self.assertEqual(lo % 0x10000, 0)
        self.assertLessEqual(hi, define("config.h", "FLASH_LEN"))
        for name, (a, b) in self.regions().items():
            block_lo, block_hi = a & ~0xFFFF, (b + 0xFFFF) & ~0xFFFF
            self.assertTrue(block_hi <= lo or block_lo >= hi, f"{name} [{a:#x}, {b:#x}) shares a 64 KiB block with the board record")

    def test_only_reader_code_names_it(self):
        allowed = {"board_record.h", "board_record_sval.c", os.path.join("updater", "update_flash.c")}
        users = set()
        for path in glob.glob(os.path.join(KB, "**", "*.[ch]"), recursive=True):
            rel = os.path.relpath(path, KB)
            if rel.startswith(os.path.join("tools", "board_id")):
                continue
            with open(path, encoding="utf-8", errors="replace") as f:
                if re.search(r"BOARD_RECORD_(BASE|END|SIZE)|0x[fF]00000\b", f.read()):
                    users.add(rel)
        self.assertEqual(users - allowed, set())

    def test_writer_never_erases(self):
        with open(os.path.join(KB, "tools", "board_id", "writer", "hal_rp2040.c"), encoding="utf-8") as f:
            text = f.read()
        self.assertNotIn("ROM_CODE('R', 'E')", text)  # flash_range_erase
        self.assertNotRegex(text, r"\b0x(20|D8|C7|60)\b.*cmd")  # no erase opcodes sent by hand


class UpdateTools(unittest.TestCase):
    def test_update_refuses_an_image_reaching_the_record(self):
        data = uf2.build(uf2.XIP_BASE, b"\0" * 0x1000) + uf2.build(uf2.XIP_BASE + br.BASE, b"\0" * 256)
        with self.assertRaises(make_update.Refused):
            make_update.uf2_to_image(data)
        with self.assertRaises(make_update.Refused):
            make_update.uf2_to_image(uf2.build(uf2.XIP_BASE, b"\0" * (br.BASE + 256)))

    def test_pair_layout_flags_the_record(self):
        data = uf2.build(uf2.XIP_BASE + br.BASE, b"\0" * 4096)
        problems = make_update.pair_layout_problems(data)
        self.assertTrue(any("board record" in p for p in problems), problems)


class RealBuilds(unittest.TestCase):
    def test_real_builds_stay_in_the_firmware_area(self):
        if not REAL:
            self.skipTest("no QMK builds given")
        for path in REAL:
            with open(path, "rb") as f:
                lo, hi = uf2.flash_extent(f.read())
            self.assertEqual(lo, 0, path)
            self.assertLessEqual(hi, 0x160000, path)
            self.assertLessEqual(hi, br.BASE, path)
            print(f"  {os.path.basename(path)}: flash [0, {hi:#x}), record block [{br.BASE:#x}, {br.BASE + br.SIZE:#x}) untouched")


if __name__ == "__main__":
    REAL = [os.path.abspath(a) for a in sys.argv[1:]]
    del sys.argv[1:]
    unittest.main(verbosity=1)
