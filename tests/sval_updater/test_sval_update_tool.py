#!/usr/bin/env python3
"""End-to-end host test for keyboards/svalboard/tools/sval_update.py.

    test_sval_update_tool.py LIB.so OUT_DIR

Drives the tool's update session against the firmware's own state machine
(updater.c, the chord, the slot flash and the image checks), built into LIB.so
by util/updater_test/run.sh with the mock die and a simulated clock from
test_updater.c. A fake HID device stands in for the board: it emulates the Sval
client wrapper and hands each wrapped VIA packet to updater_via_command().
"""
import contextlib
import ctypes
import io
import importlib.util
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / "keyboards/svalboard/tools"
sys.path.insert(0, str(TOOLS))
spec = importlib.util.spec_from_file_location("sval_update", TOOLS / "sval_update.py")
su = importlib.util.module_from_spec(spec)
spec.loader.exec_module(su)
mu = su.make_update

checks = failures = 0


def check(cond, what):
    global checks, failures
    checks += 1
    if not cond:
        failures += 1
        print(f"FAIL: {what}", file=sys.stderr)


lib = ctypes.CDLL(sys.argv[1])
lib.host_lib_reset.argtypes = [ctypes.c_int]
lib.host_via.argtypes = [ctypes.c_char_p, ctypes.c_uint32]
lib.host_passes.argtypes = [ctypes.c_int]
lib.host_state.restype = ctypes.c_int
lib.host_slot.restype = ctypes.POINTER(ctypes.c_uint8)
lib.host_commit_runs.restype = ctypes.c_int
lib.host_commit_crc.restype = ctypes.c_uint32
lib.host_lib_split.argtypes = [ctypes.c_int]
lib.host_slave_commits.restype = ctypes.c_int
lib.host_slave_commit_crc.restype = ctypes.c_uint32
lib.host_master_reboots.restype = ctypes.c_int
lib.host_slave_fw.restype = ctypes.c_uint32
lib.host_master_fw.restype = ctypes.c_uint32
lib.host_slave_state.restype = ctypes.c_int
lib.host_link_paused.restype = ctypes.c_int

BOOT2 = bytes(int(h, 16) for h in __import__("re").findall(r"0x([0-9a-f]{2})", (Path(__file__).parent / "boot2_page0.h").read_text().split("#define BOOT2_PAGE0_BYTES")[1]))


def image(length):
    body = bytearray(BOOT2 + struct.pack("<II", 0x20041000, 0x100001F7))
    body += bytes((i * 29 + 3) & 0xFF for i in range(length - len(body)))
    return bytes(body)


class FakeBoard(su.Device):
    """The board behind HID: the client wrapper, then the firmware's updater.
    The user makes the chord once the tool waits for it (sleeps between polls)
    in CONFIRM_WAIT."""

    current = None

    def __init__(self, allow_unwrapped=False):
        self.handle = None
        self.serial = "sval:fake"
        self.client = None
        self.round_trips = 0
        self.round_trip_s = 0.0
        self.issued = []
        self.waited = False
        self.chords = 0
        self.allow_unwrapped = allow_unwrapped
        FakeBoard.current = self

    def _exchange(self, packet, match):
        packet = bytes(packet).ljust(32, b"\0")
        lib.host_passes(2)  # the main loop runs between packets
        if packet[0] != su.WRAPPER:
            if not self.allow_unwrapped:
                raise AssertionError("the tool sent an unwrapped packet")
            via = ctypes.create_string_buffer(packet, 32)
            lib.host_via(via, 0)  # plain VIA: no client ID
            reply = via.raw[:32]
            if not match(reply):
                raise AssertionError(f"reply does not match: {reply.hex()}")
            self.round_trips += 1
            return reply
        cid = packet[1:5]
        if cid == bytes(4):
            new = struct.pack("<I", 0x00510000 + len(self.issued) + 1)
            self.issued.append(new)
            reply = packet[:25] + new + struct.pack("<H", 120) + b"\0"
        elif cid not in self.issued:
            reply = bytes([su.WRAPPER]) + cid + bytes([0xFF, 1]) + bytes(25)
        else:
            check(packet[5] == su.VIA_PROTO, "VIA inside the wrapper")
            via = ctypes.create_string_buffer(packet[6:32] + bytes(6), 32)
            lib.host_via(via, struct.unpack("<I", cid)[0])
            reply = bytes([su.WRAPPER]) + cid + bytes([su.VIA_PROTO]) + via.raw[:26]
        if lib.host_state() == 2 and self.waited:  # CONFIRM_WAIT and the tool is waiting: the user makes the chord
            self.waited = False
            self.chords += 1
            lib.host_chord()
        if not match(reply):
            raise AssertionError(f"reply does not match: {reply.hex()}")
        self.round_trips += 1
        return reply


class Args:
    delay = 0.0
    rebind_every = 0.0
    min_rebinds = 0
    no_commit = False
    keep = False


def run_update(svup, commit_available, rebind_every=0.0, min_rebinds=0):
    lib.host_lib_reset(1 if commit_available else 0)
    board = FakeBoard()
    args = Args()
    args.rebind_every = rebind_every
    args.min_rebinds = min_rebinds
    session = su.Session(board, args)
    rc = session.run(svup)
    return rc, board, session


slept = [0.0]
real_monotonic = __import__("time").monotonic


def fake_monotonic():
    """The tool's deadlines run on the simulated time its sleeps add."""
    return real_monotonic() + slept[0]


def fake_sleep(s):
    """Simulated time while the tool polls; the fake user acts on the next poll."""
    slept[0] += s
    lib.host_passes(max(int(s * 1000), 1))
    if FakeBoard.current:
        FakeBoard.current.waited = True


def main():
    out = Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    su.time.sleep = fake_sleep
    su.time.monotonic = fake_monotonic

    img = image(0x5100)
    seed = mu.read_key(mu.TEST_KEY_FILE)
    manifest = mu.build_manifest(img, key_id=0, hand=1, pointing_id=3, keymap_id=1, flags=0, epoch=0, storage_format=2,
                                 fw_version=7, version="tool-test")
    svup = out / "tool_test.svup"
    svup.write_bytes(manifest + mu.sign(seed, manifest, "pure") + img)

    # the stub refuses COMMIT: exit 3, image staged and verified
    rc, board, _ = run_update(svup, commit_available=False)
    check(rc == 3, f"stub commit: exit {rc}")
    check(lib.host_state() == 7, f"VERIFIED after the stub refused, state {lib.host_state()}")
    slot = bytes(lib.host_slot()[:len(img)])
    check(slot == img, "slot holds the image")
    check(lib.host_commit_runs() == 0, "no commit ran")

    # with a commit routine: COMMIT accepted, and it runs 100 ms later with the image CRC
    rc, board, _ = run_update(svup, commit_available=True)
    check(rc == 0, f"commit: exit {rc}")
    lib.host_passes(150)
    check(lib.host_commit_runs() == 1, "the commit ran once")
    check(lib.host_commit_crc() & 0xFFFFFFFF == mu.crc32_mpeg2(img[0x100:]), "commit got the image CRC over [0x100, len)")

    # client ID renewal with REBIND on every op, and the old ID refused each time
    with contextlib.redirect_stdout(io.StringIO()) as log:
        rc, board, session = run_update(svup, commit_available=False, rebind_every=1e-9)
    check(log.getvalue().count("old ID refused (OTHER_CLIENT)") == len(board.issued) - 1, "every renewal checked the old ID")
    check(rc == 3, f"rebind run: exit {rc}")
    check(len(board.issued) > 100, f"renewed {len(board.issued)} times")
    check(session.renewals == len(board.issued) - 1, f"renewals counted: {session.renewals}")
    check(bytes(lib.host_slot()[:len(img)]) == img, "slot holds the image after renewals")
    # M1 #16: --min-rebinds fails a run with too few renewals, before COMMIT
    with contextlib.redirect_stdout(io.StringIO()):
        rc, _, session = run_update(svup, commit_available=True, rebind_every=1e-9, min_rebinds=3)
    check(rc == 0 and session.renewals >= 3, f"enough renewals: exit {rc}, {session.renewals}")
    try:
        with contextlib.redirect_stdout(io.StringIO()):
            run_update(svup, commit_available=True, rebind_every=0.0, min_rebinds=1)
        check(False, "--min-rebinds 1 with no renewals passed")
    except su.UpdaterError as exc:
        check("only 0 client ID renewals" in str(exc), f"min-rebinds: {exc}")
    check(lib.host_state() == 7 and lib.host_commit_runs() == 0, "no COMMIT after a --min-rebinds failure")

    # reject (M1 #6): the protocol rejection matrix against the firmware's state machine
    lib.host_lib_reset(1)
    board = FakeBoard(allow_unwrapped=True)
    with contextlib.redirect_stdout(io.StringIO()) as log:
        rc = su.Rejects(board, svup).run()
    text = log.getvalue()
    check(rc == 0, f"reject: exit {rc}\n{text}")
    check(text.count("  PASS  ") == 26 and "FAIL" not in text, f"reject: {text.count('  PASS  ')} passed\n{text}")
    check(board.chords >= 2, f"reject: chord made {board.chords} times")
    check(lib.host_state() == 0 and lib.host_commit_runs() == 0, "reject: IDLE afterwards, nothing committed")
    for what in ("unwrapped ARM: INVALID", "BEGIN before the chord: NOT_CONFIRMED", "CHUNK ahead of the next offset: OUT_OF_ORDER",
                 "END too early: OUT_OF_ORDER", "REBIND with the wrong nonce: OTHER_CLIENT", "COMMIT with the wrong CRC: BAD_HASH",
                 "ABORT from a second client: OTHER_CLIENT", "CHUNK past the end (overrun): OVERRUN, state ERROR"):
        check(what in text, f"reject: no '{what}'")
    # reject's own check: the right status in the wrong state is a failure
    lib.host_lib_reset(0)
    rj = su.Rejects(FakeBoard(), svup)
    rj.hand = 1
    with contextlib.redirect_stdout(io.StringIO()):
        rj.expect("probe", (su.OK, bytes(22)), su.OK, su.ST_RECEIVING)
        rj.expect("probe", (su.OK, bytes(22)), su.INVALID)
        rj.expect("probe", (su.OK, bytes(22)), su.OK, su.ST_IDLE)
    check(rj.failed == 2 and rj.passed == 1, f"reject's check: {rj.passed} passed, {rj.failed} failed")
    # reject refuses to start on a busy board
    lib.host_lib_reset(0)
    board = FakeBoard(allow_unwrapped=True)
    board.op(su.MANIFEST, bytes([0, 20]) + bytes(20), hand=1)
    try:
        su.Rejects(board, svup).run()
        check(False, "reject ran on a busy board")
    except su.UpdaterError as exc:
        check("ABORT first" in str(exc), f"reject on a busy board: {exc}")

    # info and status decode against the firmware's layout
    lib.host_lib_reset(0)
    board = FakeBoard()
    info = su.decode_info(*board.op(su.INFO))
    check(info["hand"] == "right" and info["pointing"] == "pmw3389" and info["jedec"] == "ef4018", f"info {info}")
    check(info["slot_base"] == "0x800000" and info["slot_size"] == "0x160000" and info["max_image"] == "0x160000", f"info {info}")
    check(info["storage_format"] == 2 and info["flash_16mib"] and not info["settings_writes_failing"], f"info {info}")
    check(info["test_key"] and not info["release_build"], f"info {info}")
    check(not info["other_half_mismatch"], f"info {info}")
    diag = su.decode_diag(*board.op(su.DIAG, b"\0", hand=1))
    check(diag["status"] == "OK" and diag["stack_size"] == 0xAE0 and diag["stack_unused"] == 0x2A4, f"diag {diag}")
    st = su.decode_status(*board.op(su.STATUS, hand=1))
    check(st["state"] == "IDLE" and st["status"] == "OK", f"status {st}")
    check(st["other_half"] == "none" and st["other_half_id"] == 0, f"status {st}")
    st, r = board.op(su.ABORT, bytes(4), hand=1)
    check(st == su.OK, "ABORT in IDLE")

    # an image for the other half with no other half connected: refused before any op
    bad = mu.build_manifest(img, key_id=0, hand=0, pointing_id=3, keymap_id=1, flags=0, epoch=0, storage_format=2, fw_version=7, version="left")
    badf = out / "left.svup"
    badf.write_bytes(bad + mu.sign(seed, bad, "pure") + img)
    lib.host_lib_reset(0)
    try:
        with contextlib.redirect_stdout(io.StringIO()):
            su.Session(FakeBoard(), Args()).run(badf)
        check(False, "an image for an absent other half accepted")
    except su.UpdaterError as exc:
        check("presence none" in str(exc), f"no other half: {exc}")
    check(lib.host_state() == 0, "no session without the other half")

    # ---- M2: the other half, through this one ----
    left_m = mu.build_manifest(img, key_id=0, hand=0, pointing_id=1, keymap_id=1, flags=0, epoch=0, storage_format=2,
                               fw_version=9, version="left-9")
    left = out / "left9.svup"
    left.write_bytes(left_m + mu.sign(seed, left_m, "pure") + img)
    right_m = mu.build_manifest(img, key_id=0, hand=1, pointing_id=3, keymap_id=1, flags=0, epoch=0, storage_format=2,
                                fw_version=9, version="right-9")
    right = out / "right9.svup"
    right.write_bytes(right_m + mu.sign(seed, right_m, "pure") + img)
    crc = mu.crc32_mpeg2(img[0x100:])

    # update with the other half's image: staged here, relayed, committed there
    lib.host_lib_reset(1)
    lib.host_lib_split(1)
    board = FakeBoard()
    with contextlib.redirect_stdout(io.StringIO()) as log:
        rc = su.Session(board, Args()).run(left)
    text = log.getvalue()
    check(rc == 0, f"other half: exit {rc}\n{text}")
    check(lib.host_slave_commits() == 1, f"the other half committed {lib.host_slave_commits()} times")
    check(lib.host_slave_commit_crc() == crc, "the other half committed the image's CRC")
    check(lib.host_slave_fw() == 9 and lib.host_master_fw() == 2001, f"fw {lib.host_slave_fw()} / {lib.host_master_fw()}")
    check(lib.host_commit_runs() == 0 and lib.host_master_reboots() == 0, "this half did not commit")
    check(lib.host_state() == 0 and not lib.host_link_paused(), "IDLE, link running afterwards")
    for what in ("for the other (left) half", "relaying to the other half", "relayed:", "the other half took COMMIT",
                 "the other half answers again: fw 9, presence version differs", "halves now run different releases"):
        check(what in text, f"other half: no '{what}' in\n{text}")
    check("the left half without USB" in text and "on the right half, the one with USB" in text, f"chord text\n{text}")
    rl = su.decode_relay(*board.op(su.RELAY, hand=0))
    check(rl["phase"] == "done" and rl["acked"] == len(img) and rl["image_len"] == len(img), f"relay {rl}")
    st = su.decode_status(*board.op(su.STATUS, hand=1))
    check(st["other_half"] == "version differs", f"status {st}")
    inf = board.op(su.INFO, hand=0)
    check(su.decode_info(*inf)["fw_version"] == 9 and su.decode_info(*inf)["hand"] == "left", f"info --other {inf}")

    # --no-commit stops at VERIFIED here, before any relay
    lib.host_lib_reset(1)
    lib.host_lib_split(1)
    a = Args()
    a.no_commit = True
    with contextlib.redirect_stdout(io.StringIO()):
        rc = su.Session(FakeBoard(), a).run(left)
    check(rc == 0 and lib.host_state() == 7 and lib.host_slave_state() == 0, "no-commit: VERIFIED here, nothing relayed")

    # the other half's pointing device is checked at ARM (D18)
    wrong_m = mu.build_manifest(img, key_id=0, hand=0, pointing_id=3, keymap_id=1, flags=0, epoch=0, storage_format=2,
                                fw_version=9, version="left-3389")
    wrong = out / "left3389.svup"
    wrong.write_bytes(wrong_m + mu.sign(seed, wrong_m, "pure") + img)
    lib.host_lib_reset(1)
    lib.host_lib_split(1)
    try:
        with contextlib.redirect_stdout(io.StringIO()):
            su.Session(FakeBoard(), Args()).run(wrong)
        check(False, "wrong pointing for the other half accepted")
    except su.UpdaterError as exc:
        check("ARM: WRONG_HW" in str(exc), f"wrong pointing: {exc}")

    # pair: the other half, then this one; the board comes back and presence matches
    lib.host_lib_reset(1)
    lib.host_lib_split(1)
    su.reopen = lambda serial, timeout: FakeBoard()
    board = FakeBoard()
    sessions = []
    with contextlib.redirect_stdout(io.StringIO()) as log:
        rc = su.pair(board, Args(), [right, left], sessions)
    text = log.getvalue()
    check(rc == 0, f"pair: exit {rc}\n{text}")
    check(lib.host_slave_commits() == 1 and lib.host_master_reboots() == 1, "pair: each half committed once")
    check(lib.host_slave_fw() == 9 and lib.host_master_fw() == 9, f"pair: fw {lib.host_slave_fw()} / {lib.host_master_fw()}")
    check(text.index("1/2: the other (left) half") < text.index("2/2: this (right) half"), "pair: the other half first")
    check("pair: presence match" in text, f"pair:\n{text}")
    # this half reports its build's SVAL_FW_VERSION (0 on the host), not the manifest's: a warning, not a failure
    check("this half fw 0, the other half fw 9" in text and "EXTRAFLAGS=-DSVAL_FW_VERSION=N" in text, f"pair:\n{text}")
    check(len(sessions) == 2, "pair: two sessions")
    # pair refuses two images for the same half, before any op
    lib.host_lib_reset(1)
    lib.host_lib_split(1)
    try:
        su.pair(FakeBoard(), Args(), [right, right], [])
        check(False, "pair with two right images")
    except su.UpdaterError as exc:
        check("one image for each half" in str(exc), f"pair: {exc}")
    check(lib.host_state() == 0, "pair refused: nothing started")

    print(f"sval_update.py: {checks} checks, {failures} failures")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
