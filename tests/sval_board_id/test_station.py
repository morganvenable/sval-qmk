"""The station (tools/board_id/board_id.py) end to end against fake boards.

    test_station.py WRITER_HOST WRITER.uf2 WORKDIR

fake_picotool.py stands in for picotool, and the ID-writer's real logic, built
for the PC (WRITER_HOST), runs on each fake board when the station loads the
writer image, with the params the station patched into it.
"""
import csv
import io
import os
import shutil
import subprocess
import sys
import unittest
from contextlib import redirect_stdout

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
TOOL = os.path.join(ROOT, "keyboards", "svalboard", "tools", "board_id")
sys.path.insert(0, TOOL)
import board_record as br  # noqa: E402
import uf2  # noqa: E402

HOST, WRITER, WORK = None, None, None
MINI = "E46450D2A7094232"
STRAPS = {"low": (0, 0, 0), "high": (64, 64, 64), "floating": (64, 0, 30), "unstable": (10, 3, 5)}


class Station(unittest.TestCase):
    def setUp(self):
        self.dir = os.path.join(WORK, self.id().split(".")[-1])
        shutil.rmtree(self.dir, ignore_errors=True)
        self.fake = os.path.join(self.dir, "boards")
        self.log = os.path.join(self.dir, "log")
        os.makedirs(self.fake)
        open(os.path.join(self.fake, "devices.txt"), "w").close()
        open(os.path.join(self.fake, "calls.txt"), "w").close()
        os.environ["BOARD_ID_WRITER_HOST"] = HOST
        os.environ["BOARD_ID_WRITER_UF2"] = WRITER
        import board_id
        self.tool = board_id
        self.batch()

    def batch(self, model="SvalMCUMini V1.0", revision="v1.0b0", lot="2026-10-A"):
        self.main("batch", "--model", model, "--revision", revision, "--lot", lot, "--station", "bench1")

    def main(self, *args):
        out = io.StringIO()
        with redirect_stdout(out):
            try:
                code = self.tool.main(["--dir", self.log, "--fake", self.fake, *args])
            except SystemExit as e:
                code = e.code
                if isinstance(code, str):  # sys.exit("message"): printed, status 1
                    print(code)
                    code = 1
        self.out = out.getvalue()
        return code

    def board(self, serial=MINI, strap="low", jedec=0x18, uid=None, present=True):
        d = os.path.join(self.fake, serial)
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, "block.bin"), "wb") as f:
            f.write(b"\xff" * br.SIZE)
        with open(os.path.join(d, "chip.bin"), "wb") as f:
            f.write(bytes([jedec]) + bytes.fromhex(uid or serial) + bytes(STRAPS[strap]))
        if present:
            with open(os.path.join(self.fake, "devices.txt"), "a") as f:
                f.write(serial + "\n")
        return d

    def block(self, serial=MINI):
        with open(os.path.join(self.fake, serial, "block.bin"), "rb") as f:
            return f.read()

    def calls(self):
        with open(os.path.join(self.fake, "calls.txt")) as f:
            return [line.split() for line in f if line.strip()]

    def rows(self):
        path = os.path.join(self.log, "board_id_log.csv")
        if not os.path.exists(path):
            return []
        with open(path, newline="") as f:
            return list(csv.DictReader(f))

    def run_once(self, *extra):
        return self.main("run", "--once", "--no-wait", "--settle", "0", *extra)

    def assert_safe_calls(self):
        for c in self.calls():
            self.assertIn("--ser", c)
            self.assertFalse({"-f", "-F", "--force", "--force-no-reboot"} & set(c), c)
            self.assertNotIn("erase", c)

    # ---- cases -------------------------------------------------------------------

    def test_new_board_passes(self):
        self.board()
        self.assertEqual(self.run_once(), 0, self.out)
        s = br.scan(self.block())
        self.assertEqual((s["identities"], s["newest"]), (1, 0))
        cur = s["current"]
        self.assertEqual((cur["model"], cur["revision"], cur["lot"], cur["uid"], cur["strap"], cur["expected"]),
                         ("SvalMCUMini V1.0", "v1.0b0", "2026-10-A", MINI, "low", "low"))
        self.assertTrue(cur["station"].startswith("bench1 board_id/"))
        row = self.rows()[-1]
        self.assertEqual((row["result"], row["uid"], row["strap"], row["page"]), ("PASS", MINI, "low", "0"))
        self.assertIn("PASS", self.out)
        self.assert_safe_calls()
        # The writer image went to RAM only; nothing was written to flash by a load.
        self.assertFalse(os.path.exists(os.path.join(self.fake, MINI, "flash_writes.txt")))

    def test_second_time_refused_without_loading(self):
        self.board()
        self.run_once()
        before = self.block()
        n = len(self.calls())
        self.assertEqual(self.run_once(), 1)
        self.assertEqual(self.rows()[-1]["result"], "REFUSED_HAS_RECORD")
        self.assertEqual(self.block(), before)
        self.assertEqual([c[0] for c in self.calls()[n:]], ["save"])  # read only

    def test_force_appends_and_keeps_history(self):
        self.board()
        self.run_once()
        first = self.block()[:256]
        self.batch(lot="2026-10-B")
        self.assertEqual(self.run_once("--force"), 0, self.out)
        s = br.scan(self.block())
        self.assertEqual((s["identities"], s["newest"]), (2, 1))
        self.assertEqual(self.block()[:256], first)
        self.assertEqual(s["current"]["lot"], "2026-10-B")

    def test_strap_mismatch_writes_nothing(self):
        self.board(strap="high")
        self.assertEqual(self.run_once(), 1)
        self.assertEqual(self.rows()[-1]["result"], "FAIL_STRAP_MISMATCH")
        self.assertIn("GP3 reads high", self.rows()[-1]["detail"])
        self.assertEqual(self.block(), b"\xff" * br.SIZE)

    def test_v30_floating_matches(self):
        self.batch(model="V3.0", revision="", lot="old")
        self.board(strap="floating")
        self.assertEqual(self.run_once(), 0, self.out)
        self.assertEqual(br.scan(self.block())["current"]["strap"], "floating")

    def test_unstable_strap_refused_unless_strap_any(self):
        self.board(strap="unstable")
        self.assertEqual(self.run_once(), 1)
        self.assertEqual(self.rows()[-1]["result"], "FAIL_STRAP_UNSTABLE")
        self.assertEqual(self.run_once("--strap-any"), 0, self.out)
        cur = br.scan(self.block())["current"]
        self.assertEqual((cur["strap"], cur["strap_counts"]), ("unstable", [10, 3, 5]))
        self.assertNotIn("expected", cur)

    def test_small_flash_refused(self):
        self.board(jedec=0x15)
        self.assertEqual(self.run_once(), 1)
        self.assertEqual(self.rows()[-1]["result"], "FAIL_NOT_16MB")
        self.assertEqual(self.block(), b"\xff" * br.SIZE)

    def test_no_unique_id_refused(self):
        self.board(uid="FFFFFFFFFFFFFFFF")
        self.assertEqual(self.run_once(), 1)
        self.assertEqual(self.rows()[-1]["result"], "FAIL_NO_UID")

    def test_dry_run_writes_nothing(self):
        self.board()
        self.assertEqual(self.run_once("--dry"), 0, self.out)
        self.assertEqual(self.rows()[-1]["result"], "PASS_DRY")
        self.assertEqual(self.block(), b"\xff" * br.SIZE)

    def test_firmware_after_record(self):
        self.board()
        fw = os.path.join(self.dir, "fw.uf2")
        with open(fw, "wb") as f:
            f.write(uf2.build(uf2.XIP_BASE, b"\x01" * 0x2000))
        self.assertEqual(self.run_once("--firmware", fw), 0, self.out)
        row = self.rows()[-1]
        self.assertEqual((row["result"], row["firmware"]), ("PASS", "fw.uf2"))
        with open(os.path.join(self.fake, MINI, "flash_writes.txt")) as f:
            offs = [int(line.split()[0], 16) for line in f]
        self.assertTrue(offs and max(offs) < 0x160000)
        self.assertEqual(br.scan(self.block())["identities"], 1)
        with open(os.path.join(self.fake, "devices.txt")) as f:
            self.assertEqual(f.read().strip(), "")  # rebooted into the firmware

    def test_firmware_into_record_refused_before_anything(self):
        self.board()
        fw = os.path.join(self.dir, "bad.uf2")
        with open(fw, "wb") as f:
            f.write(uf2.build(uf2.XIP_BASE + br.BASE, b"\0" * 256))
        self.assertNotEqual(self.run_once("--firmware", fw), 0)
        self.assertIn("Refusing the firmware", self.out)
        self.assertEqual(self.calls(), [])
        big = os.path.join(self.dir, "big.uf2")
        with open(big, "wb") as f:
            f.write(uf2.build(uf2.XIP_BASE, b"\0" * 0x161000))
        self.assertEqual(self.main("check", big), 1)
        self.assertEqual(self.main("check", fw), 1)
        good = os.path.join(self.dir, "good.uf2")
        with open(good, "wb") as f:
            f.write(uf2.build(uf2.XIP_BASE, b"\0" * 0x1000))
        self.assertEqual(self.main("check", good), 0)

    def test_deny_list_untouched(self):
        self.board(serial="E465B0621736062B")
        self.assertEqual(self.run_once(), 1)
        self.assertEqual(self.rows()[-1]["result"], "DENIED")
        self.assertEqual(self.calls(), [])

    def test_generic_usb_serial_pins_to_flash_id(self):
        # Some boot ROMs list a generic USB serial in BOOTSEL; picotool --ser matches the flash ID.
        self.board(present=False)
        with open(os.path.join(self.fake, "devices.txt"), "w") as f:
            f.write("E0C9125B0D9B\n")
        with open(os.path.join(self.fake, "pnp_ids.txt"), "w") as f:
            f.write("E0C9125B0D9B " + MINI + "\n")
        self.assertEqual(self.run_once(), 0, self.out)
        row = self.rows()[-1]
        self.assertEqual((row["result"], row["bootsel_serial"], row["uid"]), ("PASS", "E0C9125B0D9B", MINI))
        self.assertTrue(all(c[c.index("--ser") + 1] == MINI for c in self.calls()))

    def test_deny_list_by_flash_id(self):
        self.board(serial="E465B0621736062B", present=False)
        with open(os.path.join(self.fake, "devices.txt"), "w") as f:
            f.write("E0C9125B0D9B\n")
        with open(os.path.join(self.fake, "pnp_ids.txt"), "w") as f:
            f.write("E0C9125B0D9B E465B0621736062B\n")
        self.assertEqual(self.run_once(), 1)
        self.assertEqual(self.rows()[-1]["result"], "DENIED")
        self.assertEqual(self.calls(), [])

    def test_two_boards_wait(self):
        self.board()
        self.board(serial="E6614103E73B5A2F")
        self.assertEqual(self.run_once(), 1)
        self.assertIn("leave only one", self.out)
        self.assertEqual(self.calls(), [])
        self.assertEqual(self.main("read"), 1)

    def test_read_shows_history(self):
        self.board()
        self.run_once()
        self.run_once("--force")
        self.assertEqual(self.main("read"), 0)
        self.assertIn("2 identity record(s)", self.out)
        self.assertIn("page 1 (current)", self.out)

    def test_unpatched_writer_does_nothing(self):
        d = self.board()
        with open(WRITER, "rb") as f:
            image = f.read()
        params = b"".join(p for a, p in uf2.blocks(image) if br.PARAMS_ADDR <= a < br.PARAMS_ADDR + 256)
        with open(os.path.join(d, "p.bin"), "wb") as f:
            f.write(params)
        subprocess.run([HOST, d, os.path.join(d, "p.bin"), os.path.join(d, "r.bin")], check=True)
        with open(os.path.join(d, "r.bin"), "rb") as f:
            rep = br.parse_report(f.read())
        self.assertEqual(rep["status"], "BAD_PARAMS")
        self.assertEqual(self.block(), b"\xff" * br.SIZE)


if __name__ == "__main__":
    HOST, WRITER, WORK = (os.path.abspath(a) for a in sys.argv[1:4])
    del sys.argv[1:4]
    unittest.main(verbosity=1)
