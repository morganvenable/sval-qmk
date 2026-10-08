#!/usr/bin/env python3
"""Svalboard in-firmware updater, host side (M1: the half with USB, over USB).

Needs hidapi (the 'hid' module) and a board running an SVAL_UPDATER=yes build.
Close other configuration clients (Keybard) first: raw HID replies go to every
open handle.

    sval_update.py list
    sval_update.py info
    sval_update.py status
    sval_update.py diag [--clear]
    sval_update.py abort [--nonce N]
    sval_update.py update FILE.svup [--delay S] [--rebind-every S] [--min-rebinds N]
                                    [--no-commit] [--halt POINT [--halt-unfed]]
    sval_update.py reject FILE.svup

update sends the manifest, prints the manifest hash you confirm, waits for the
chord (Index South + Middle South held for 1 s on this half), then erases,
sends the image, verifies and commits. The client ID is renewed every
--rebind-every seconds (50, as Keybard does) and the session moved to it with
REBIND; after each REBIND the old ID must be refused. The number of renewals is
printed; --min-rebinds N fails the run (before COMMIT) if there were fewer (M1
#16: use a short --rebind-every, e.g. 5, and --min-rebinds 3). --delay S waits
S seconds between chunks (slow-transfer test; the session times out after 30 s
without an op, so --delay 31 gives TIMEOUT).

reject (M1 #6) runs the protocol rejection matrix with FILE.svup, an image
this half accepts: unwrapped packets, a second client ID, wrong nonces,
BEGIN before the chord, an out-of-order CHUNK, END too early, ABORT and
REBIND from the wrong client, COMMIT with the wrong CRC, then an overrun. It
needs the chord twice and checks each status and the state after it. It never
commits. The image-level rejections are .svup files from make_update.py
(--unsafe-allow-bad-image) sent with update.

diag (M1 #13) reports how much of main's stack has never been used since boot
and the longest updater pass in ms; --clear restarts the longest-pass record.

--halt POINT needs an SVAL_UPDATE_TEST_HOOKS build (M1 #8-#10): the commit
stops for good at POINT (invalidated, first-erase, mid-program, last-sector,
page0; or fault / fault-erased for a HardFault after the invalidation or after
the first erase). A halt clears RAM as the commit's reset does, then spins
feeding the watchdog, so the board hangs until it is unplugged, or with
--halt-unfed it lets the watchdog reset it within 8 s. A fault point resets at
once (well under 1 s) through the RAM vector table; a reset only after about
8 s means the RAM vector table was not used. Up to last-sector the board comes
back as RPI-RP2 (page 0 is not valid); recover it with flash.sh.

The protocol is documented at the top of keyboards/svalboard/updater/updater.c.
"""
import argparse
import hashlib
import json
import secrets
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import make_update  # noqa: E402  (same directory: .svup parsing and the CRC)

CHANNEL = 0x55
SET_VALUE = 0x07
WRAPPER, VIA_PROTO, WRAPPER_ERROR = 0xDD, 0xFE, 0xFF
HAND_SELF = 0xFF
INFO, MANIFEST, ARM, BEGIN, CHUNK, END, STATUS, COMMIT, ABORT, REBIND, TEST_HALT, DIAG = range(12)
HALT_POINTS = {"invalidated": 1, "first-erase": 2, "mid-program": 3, "last-sector": 4, "page0": 5, "fault": 6,
               "fault-erased": 7}
MANIFEST_PIECE, CHUNK_PIECE = 20, 18

STATUS_NAMES = ["OK", "INVALID", "UNAVAILABLE", "FLASH_ERR", "ACCEPTED", "BUSY", "NOT_CONFIRMED", "WRONG_HW", "BAD_SIG",
                "BAD_HASH", "BAD_IMAGE", "EPOCH", "OUT_OF_ORDER", "OVERRUN", "TOO_LARGE", "TIMEOUT", "OTHER_CLIENT",
                "UNSUPPORTED", "STORAGE"]
STATE_NAMES = ["IDLE", "MANIFEST_LOADING", "CONFIRM_WAIT", "VERIFYING_MANIFEST", "ERASING", "RECEIVING",
               "VERIFYING_IMAGE", "VERIFIED", "COMMITTING", "ERROR"]
OK, INVALID, ACCEPTED, NOT_CONFIRMED, BAD_HASH = 0, 1, 4, 6, 9
OUT_OF_ORDER, OVERRUN, OTHER_CLIENT, UNSUPPORTED = 12, 13, 16, 17
ST_IDLE, ST_MANIFEST_LOADING, ST_CONFIRM_WAIT, ST_RECEIVING, ST_VERIFIED, ST_ERROR = 0, 1, 2, 5, 7, 9
HANDS = {0: "left", 1: "right"}
POINTING = {v: k for k, v in make_update.POINTING.items()}


def status_name(code):
    return STATUS_NAMES[code] if code < len(STATUS_NAMES) else f"status {code}"


def state_name(code):
    return STATE_NAMES[code] if code < len(STATE_NAMES) else f"state {code}"


class UpdaterError(RuntimeError):
    pass


class ClientIdExpired(RuntimeError):
    pass


def devices():
    import hid
    return [d for d in hid.enumerate() if d.get("usage_page") == 0xFF61 and d.get("usage") == 0x62]


class Device:
    def __init__(self, serial=None):
        import hid
        matches = [d for d in devices() if serial is None or d.get("serial_number") == serial]
        if len(matches) != 1:
            raise RuntimeError(f"Expected one Svalboard, found {len(matches)}; use --serial (see list)")
        self.serial = matches[0].get("serial_number")
        self.handle = hid.device()
        self.handle.open_path(matches[0]["path"])
        self.client = None
        self.round_trips = 0
        self.round_trip_s = 0.0

    def close(self):
        if self.handle:
            self.handle.close()
            self.handle = None

    def _exchange(self, packet, match):
        packet = bytes(packet).ljust(32, b"\0")
        if self.handle.write(b"\0" + packet) != 33:
            raise RuntimeError("Incomplete HID write")
        t0 = time.monotonic()
        deadline = t0 + 2.0
        while time.monotonic() < deadline:
            reply = bytes(self.handle.read(32, 200))
            if len(reply) == 32 and match(reply):
                self.round_trips += 1
                self.round_trip_s += time.monotonic() - t0
                return reply
        raise RuntimeError(f"No reply to {packet[:12].hex()}")

    def new_client(self):
        """A fresh client ID from the Sval client wrapper, without adopting it."""
        nonce = secrets.token_bytes(20)
        r = self._exchange(bytes([WRAPPER]) + bytes(4) + nonce, lambda r: r[0] == WRAPPER and r[1:5] == bytes(4) and r[5:25] == nonce)
        cid = r[25:29]
        if cid in (bytes(4), b"\xFF" * 4):
            raise RuntimeError("Invalid client bootstrap response")
        return cid

    def bootstrap(self):
        """A fresh client ID, used for the ops from now on."""
        self.client = self.new_client()
        return self.client

    def unwrapped_op(self, op, args=b"", hand=HAND_SELF):
        """One updater op as a plain VIA packet, without the client wrapper (reject tests)."""
        body = bytes([SET_VALUE, CHANNEL, op, hand]) + bytes(args)
        r = self._exchange(body, lambda r: r[0:3] == body[:3])
        return r[3], r[4:26]

    def op(self, op, args=b"", hand=HAND_SELF, client=None):
        """One updater op in the client wrapper: (status, the 22 reply bytes after it)."""
        cid = client or self.client or self.bootstrap()
        body = bytes([SET_VALUE, CHANNEL, op, hand]) + bytes(args)
        if len(body) > 26:
            raise ValueError("updater request too long")
        r = self._exchange(bytes([WRAPPER]) + cid + bytes([VIA_PROTO]) + body,
                           lambda r: r[0] == WRAPPER and r[1:5] == cid and (r[5] == WRAPPER_ERROR or r[6:9] == body[:3]))
        if r[5] == WRAPPER_ERROR:
            raise ClientIdExpired(f"client wrapper error {r[6]} for client {cid.hex()}")
        return r[9], r[10:32]


def decode_info(st, r):
    return dict(status=status_name(st), state=state_name(r[0]), protocol=r[1],
                slot_base=f"{struct.unpack_from('<H', r, 2)[0] * 4096:#x}",
                slot_size=f"{struct.unpack_from('<H', r, 4)[0] * 4096:#x}",
                max_image=f"{struct.unpack_from('<H', r, 6)[0] * 4096:#x}",
                jedec=r[8:11].hex(), pointing=POINTING.get(r[11], r[11]), hand=HANDS.get(r[12], r[12]),
                hand_id=r[12], pointing_id=r[11], fw_version=struct.unpack_from("<I", r, 13)[0], storage_format=r[17],
                security_epoch=struct.unpack_from("<H", r, 18)[0], flash_16mib=bool(r[20] & 1),
                settings_writes_failing=bool(r[20] & 2), release_build=bool(r[20] & 4), test_hooks=bool(r[20] & 8),
                test_key=bool(r[20] & 16), last_error=status_name(r[21]))


def decode_diag(st, r):
    return dict(status=status_name(st), stack_unused=struct.unpack_from("<H", r, 0)[0],
                stack_size=struct.unpack_from("<H", r, 2)[0], longest_pass_ms=struct.unpack_from("<H", r, 4)[0],
                longest_pass_state=state_name(r[6]))


def decode_status(st, r):
    return dict(status=status_name(st), state=state_name(r[0]), state_id=r[0], staged=r[1] | r[2] << 8 | r[3] << 16,
                image_len=r[4] | r[5] << 8 | r[6] << 16, sectors_erased=struct.unpack_from("<H", r, 7)[0],
                sectors_to_erase=struct.unpack_from("<H", r, 9)[0], last_error=status_name(r[11]), last_error_id=r[11],
                signature_ms=struct.unpack_from("<H", r, 12)[0], verify_ms=struct.unpack_from("<H", r, 14)[0],
                erase_ms_max=struct.unpack_from("<H", r, 16)[0], crc_lo16=struct.unpack_from("<H", r, 18)[0],
                chord_made=bool(r[20] & 1), session_bound=bool(r[20] & 2), settings_writes_failing=bool(r[20] & 4))


class Session:
    def __init__(self, dev, args):
        self.dev = dev
        self.args = args
        self.hand = None
        self.nonce = None
        self.renewed = time.monotonic()
        self.renewals = 0

    def need(self, what, got, *want):
        st, r = got
        if st not in want:
            extra = ""
            if st != OK:
                s = decode_status(*self.dev.op(STATUS, hand=self.hand))
                extra = f" (state {s['state']}, last error {s['last_error']})"
            raise UpdaterError(f"{what}: {status_name(st)}{extra}")
        return r

    def status(self):
        return decode_status(*self.dev.op(STATUS, hand=self.hand))

    def nonce_op(self, op, extra=b""):
        return self.dev.op(op, struct.pack("<I", self.nonce) + extra, hand=self.hand)

    def renew(self):
        """New client ID, then REBIND; the old ID must then be refused (D3, M1 #16)."""
        old = self.dev.client
        self.dev.bootstrap()
        self.need("REBIND", self.nonce_op(REBIND), OK)
        # A repeat of the first byte is harmless to a live session; from the old ID it must be refused.
        try:
            st, _ = self.dev.op(CHUNK, bytes([0, 0, 0, 1, self.image[0]]), hand=self.hand, client=old)
            how = status_name(st)
        except ClientIdExpired:
            st, how = OTHER_CLIENT, "expired at the client wrapper"
        if st != OTHER_CLIENT:
            raise UpdaterError(f"old client ID {old.hex()} was not refused after REBIND: {how}")
        print(f"  renewed client ID {old.hex()} -> {self.dev.client.hex()}; old ID refused ({how})")
        self.renewed = time.monotonic()
        self.renewals += 1

    def maybe_renew(self):
        if self.args.rebind_every and time.monotonic() - self.renewed >= self.args.rebind_every:
            self.renew()

    def poll(self, until, what, timeout):
        deadline = time.monotonic() + timeout
        last = None
        while time.monotonic() < deadline:
            self.maybe_renew()
            s = self.status()
            if s["state_id"] == ST_ERROR:
                raise UpdaterError(f"{what}: {s['last_error']}")
            if until(s):
                return s
            if s["state"] != last:
                last = s["state"]
            time.sleep(0.2)
        raise UpdaterError(f"{what}: no progress in {timeout} s")

    def run(self, path):
        data = Path(path).read_bytes()
        manifest, sig, image = make_update.parse_svup(data)
        self.image = image
        m = make_update.parse_manifest(manifest)
        mhash = hashlib.sha512(manifest).digest()[:4]
        crc_body = make_update.crc32_mpeg2(image[0x100:])
        version = m["version"].rstrip(b"\0").decode("ascii", "replace")

        info = decode_info(*self.dev.op(INFO))
        self.hand = info["hand_id"]
        if getattr(self.args, "halt", None) and not info["test_hooks"]:
            raise UpdaterError("--halt needs an SVAL_UPDATE_TEST_HOOKS build on the board")
        print(f"board: {info['hand']} half, pointing {info['pointing']}, JEDEC {info['jedec']}, state {info['state']}, "
              f"status {info['status']}")
        if m["hand"] != self.hand or m["pointing_id"] != info["pointing_id"]:
            print(f"warning: image is for hand {m['hand']}, pointing {m['pointing_id']}; the board will refuse it")
        if m["key_id"] == 0 and not info["test_key"]:
            print("warning: image is signed with the TEST-ONLY key, which this build does not accept "
                  "(build with SVAL_UPDATE_TEST_KEY=yes); the board will refuse it")
        print(f"image: {version!r} ({m['fw_version']}), {len(image)} B, key_id {m['key_id']}, flags {m['flags']:#x}")

        t_start = time.monotonic()
        blob = manifest + sig
        for off in range(0, len(blob), MANIFEST_PIECE):
            piece = blob[off:off + MANIFEST_PIECE]
            r = self.need("MANIFEST", self.dev.op(MANIFEST, bytes([off, len(piece)]) + piece, hand=self.hand), OK)
            if r[0] != off + len(piece):
                raise UpdaterError(f"MANIFEST: board has {r[0]} bytes, expected {off + len(piece)}")
        r = self.need("ARM", self.dev.op(ARM, hand=self.hand), OK)
        self.nonce = struct.unpack_from("<I", r, 0)[0]
        self.renewed = time.monotonic()
        if r[4:8] != mhash:
            raise UpdaterError(f"ARM: board's manifest hash {r[4:8].hex()} differs from this file's {mhash.hex()}")
        print()
        print(f"  MANIFEST HASH  {mhash.hex()}   ({version}, {len(image)} bytes)")
        print("  Check this matches what you expect, then confirm on the keyboard: hold Index South + Middle South")
        print("  for 1 second on this half (M and , on the right; C and V on the left). The LED blinks blue.")
        print()
        self.poll(lambda s: s["chord_made"], "waiting for the chord", 35)
        print("chord recognised")
        self.need("BEGIN", self.nonce_op(BEGIN), ACCEPTED)
        s = self.poll(lambda s: s["state_id"] == ST_RECEIVING, "verifying the manifest and erasing", 120)
        print(f"signature checked in {s['signature_ms']} ms; erased {s['sectors_to_erase']} sectors, longest {s['erase_ms_max']} ms")

        t_send = time.monotonic()
        off, step = 0, max(len(image) // 20, 1)
        next_report = step
        while off < len(image):
            self.maybe_renew()
            n = min(CHUNK_PIECE, len(image) - off)
            st, r = self.dev.op(CHUNK, struct.pack("<I", off)[:3] + bytes([n]) + image[off:off + n], hand=self.hand)
            nxt = r[0] | r[1] << 8 | r[2] << 16
            if st == OK:
                off = nxt
            elif st == OUT_OF_ORDER and nxt <= len(image):
                print(f"  resync: board expects offset {nxt}, not {off}")
                off = nxt
            else:
                self.need(f"CHUNK at {off}", (st, r), OK)
            if off >= next_report:
                print(f"  {off * 100 // len(image)}% ({off} B)")
                next_report += step
            if self.args.delay:
                time.sleep(self.args.delay)
        t_sent = time.monotonic()
        self.need("END", self.nonce_op(END), ACCEPTED)
        s = self.poll(lambda s: s["state_id"] == ST_VERIFIED, "verifying the image", 60)
        if s["crc_lo16"] != crc_body & 0xFFFF:
            raise UpdaterError(f"board's image CRC {s['crc_lo16']:#06x} differs from this file's {crc_body & 0xFFFF:#06x}")
        summary = dict(transfer_s=round(t_sent - t_send, 2), total_s=round(time.monotonic() - t_start, 2),
                       round_trips=self.dev.round_trips,
                       mean_round_trip_ms=round(1000 * self.dev.round_trip_s / max(self.dev.round_trips, 1), 2),
                       signature_ms=s["signature_ms"], verify_ms=s["verify_ms"], erase_ms_max=s["erase_ms_max"],
                       rebinds=self.renewals)
        print("verified: " + json.dumps(summary))
        min_rebinds = getattr(self.args, "min_rebinds", 0) or 0
        if self.renewals < min_rebinds:
            raise UpdaterError(f"only {self.renewals} client ID renewals with REBIND, --min-rebinds {min_rebinds} "
                               "(use a shorter --rebind-every)")
        if self.args.no_commit:
            print("--no-commit: leaving the verified image staged (ABORT to clear the session)")
            return 0
        if getattr(self.args, "halt", None):
            fed = not self.args.halt_unfed
            self.need("TEST_HALT", self.nonce_op(TEST_HALT, bytes([HALT_POINTS[self.args.halt], int(fed)])), OK)
            print(f"test hook: the commit will halt at {self.args.halt}, "
                  f"{'feeding the watchdog (unplug to end it)' if fed else 'not feeding the watchdog (reset within 8 s)'}")
        st, r = self.nonce_op(COMMIT, struct.pack("<H", crc_body & 0xFFFF))
        if st == UNSUPPORTED:
            print("COMMIT: UNSUPPORTED - this build cannot commit; the image is staged and verified")
            return 3
        self.need("COMMIT", (st, r), ACCEPTED)
        print("committing: the board writes the new image and resets")
        return 0

    def abort(self):
        if self.nonce is not None:
            try:
                st, _ = self.nonce_op(ABORT)
                print(f"ABORT: {status_name(st)}", file=sys.stderr)
            except Exception as exc:  # the board may be gone
                print(f"ABORT failed: {exc}", file=sys.stderr)


class Rejects:
    """M1 #6, the protocol rows: each malformed or misdirected request must get
    its expected status and leave the state as it was (or latch ERROR, for an
    overrun). Two sessions, each confirmed with the chord; nothing is committed."""

    def __init__(self, dev, path):
        self.dev = dev
        manifest, sig, self.image = make_update.parse_svup(Path(path).read_bytes())
        self.blob = manifest + sig
        self.crc_body = make_update.crc32_mpeg2(self.image[0x100:])
        self.passed = self.failed = 0

    def state(self):
        return decode_status(*self.dev.op(STATUS, hand=self.hand))

    def expect(self, what, got, want, state=None):
        st, r = got
        s = self.state()
        ok = st == want and (state is None or s["state_id"] == state)
        self.passed += ok
        self.failed += not ok
        print(f"  {'PASS' if ok else 'FAIL'}  {what}: {status_name(st)}, state {s['state']}"
              + ("" if ok else f" (expected {status_name(want)}" + ("" if state is None else f", {state_name(state)}") + ")"))
        return r

    def load(self, client):
        for off in range(0, len(self.blob), MANIFEST_PIECE):
            piece = self.blob[off:off + MANIFEST_PIECE]
            st, _ = self.dev.op(MANIFEST, bytes([off, len(piece)]) + piece, hand=self.hand, client=client)
            if st != OK:
                raise UpdaterError(f"MANIFEST: {status_name(st)}")

    def chunk(self, client, off, n, data=None):
        data = self.image[off:off + n] if data is None else data
        return self.dev.op(CHUNK, struct.pack("<I", off)[:3] + bytes([n]) + bytes(data).ljust(n, b"\0"), hand=self.hand, client=client)

    def send_image(self, client, end):
        off = 0
        while off < end:
            n = min(CHUNK_PIECE, end - off)
            st, _ = self.chunk(client, off, n)
            if st != OK:
                raise UpdaterError(f"CHUNK at {off}: {status_name(st)}")
            off += n

    def nonce_op(self, op, nonce, client, extra=b""):
        return self.dev.op(op, struct.pack("<I", nonce & 0xFFFFFFFF) + extra, hand=self.hand, client=client)

    def poll(self, until, what, timeout):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            s = self.state()
            if s["state_id"] == ST_ERROR:
                raise UpdaterError(f"{what}: {s['last_error']}")
            if until(s):
                return s
            time.sleep(0.2)
        raise UpdaterError(f"{what}: no progress in {timeout} s")

    def arm(self, a):
        r = self.expect("ARM from the loading client", self.dev.op(ARM, hand=self.hand, client=a), OK, ST_CONFIRM_WAIT)
        return struct.unpack_from("<I", r, 0)[0]

    def chord(self, a, nonce):
        print("  -> confirm on the keyboard now: hold Index South + Middle South for 1 s on this half")
        self.poll(lambda s: s["chord_made"], "waiting for the chord", 35)
        self.expect("BEGIN after the chord", self.nonce_op(BEGIN, nonce, a), ACCEPTED)
        self.poll(lambda s: s["state_id"] == ST_RECEIVING, "verifying the manifest and erasing", 120)

    def run(self):
        info = decode_info(*self.dev.op(INFO))
        self.hand = info["hand_id"]
        if info["state"] != "IDLE":
            raise UpdaterError(f"the board is in {info['state']}; ABORT first")
        a = self.dev.bootstrap()
        b = self.dev.new_client()

        print("session 1: unwrapped packets, a second client, wrong nonces, order")
        self.expect("unwrapped INFO", self.dev.unwrapped_op(INFO), OK, ST_IDLE)
        self.expect("unwrapped MANIFEST", self.dev.unwrapped_op(MANIFEST, bytes([0, MANIFEST_PIECE]) + self.blob[:MANIFEST_PIECE], hand=self.hand), INVALID, ST_IDLE)
        self.load(a)
        self.expect("MANIFEST from a second client", self.dev.op(MANIFEST, bytes([0, MANIFEST_PIECE]) + self.blob[:MANIFEST_PIECE], hand=self.hand, client=b), OTHER_CLIENT, ST_MANIFEST_LOADING)
        self.expect("ARM from a second client", self.dev.op(ARM, hand=self.hand, client=b), OTHER_CLIENT, ST_MANIFEST_LOADING)
        self.expect("unwrapped ARM", self.dev.unwrapped_op(ARM, hand=self.hand), INVALID, ST_MANIFEST_LOADING)
        self.expect("ABORT from a second client", self.nonce_op(ABORT, 0, b), OTHER_CLIENT, ST_MANIFEST_LOADING)
        nonce = self.arm(a)
        self.expect("REBIND with the wrong nonce", self.nonce_op(REBIND, nonce ^ 1, b), OTHER_CLIENT, ST_CONFIRM_WAIT)
        self.expect("BEGIN with the wrong nonce", self.nonce_op(BEGIN, nonce ^ 1, a), OTHER_CLIENT, ST_CONFIRM_WAIT)
        self.expect("BEGIN from a second client", self.nonce_op(BEGIN, nonce, b), OTHER_CLIENT, ST_CONFIRM_WAIT)
        self.expect("BEGIN before the chord", self.nonce_op(BEGIN, nonce, a), NOT_CONFIRMED, ST_CONFIRM_WAIT)
        self.chord(a, nonce)
        r = self.expect("CHUNK ahead of the next offset", self.chunk(a, CHUNK_PIECE, CHUNK_PIECE), OUT_OF_ORDER, ST_RECEIVING)
        if r[0] | r[1] << 8 | r[2] << 16:
            self.failed += 1
            print("  FAIL  ... and it did not report offset 0 as the next one")
        self.expect("END too early", self.nonce_op(END, nonce, a), OUT_OF_ORDER, ST_RECEIVING)
        self.expect("CHUNK from a second client", self.chunk(b, 0, CHUNK_PIECE), OTHER_CLIENT, ST_RECEIVING)
        self.expect("unwrapped CHUNK", self.dev.unwrapped_op(CHUNK, bytes([0, 0, 0, CHUNK_PIECE]) + self.image[:CHUNK_PIECE], hand=self.hand), INVALID, ST_RECEIVING)
        self.expect("ABORT with the wrong nonce", self.nonce_op(ABORT, nonce ^ 1, a), OTHER_CLIENT, ST_RECEIVING)
        self.expect("ABORT from a second client", self.nonce_op(ABORT, nonce, b), OTHER_CLIENT, ST_RECEIVING)
        self.send_image(a, len(self.image))
        self.expect("END", self.nonce_op(END, nonce, a), ACCEPTED)
        self.poll(lambda s: s["state_id"] == ST_VERIFIED, "verifying the image", 60)
        crc = struct.pack("<H", self.crc_body & 0xFFFF)
        bad_crc = struct.pack("<H", (self.crc_body ^ 1) & 0xFFFF)
        self.expect("COMMIT with the wrong CRC", self.nonce_op(COMMIT, nonce, a, bad_crc), BAD_HASH, ST_VERIFIED)
        self.expect("COMMIT from a second client", self.nonce_op(COMMIT, nonce, b, crc), OTHER_CLIENT, ST_VERIFIED)
        self.expect("ABORT from the session", self.nonce_op(ABORT, nonce, a), OK, ST_IDLE)

        print("session 2: overrun")
        self.load(a)
        nonce = self.arm(a)
        self.chord(a, nonce)
        # all but the last byte, then 2 bytes from there: one more than is left
        self.send_image(a, len(self.image) - 1)
        self.expect("CHUNK past the end (overrun)", self.chunk(a, len(self.image) - 1, 2, self.image[-1:] + b"\0"), OVERRUN, ST_ERROR)
        print("  -> the LEDs should now blink red (error latched)")
        self.expect("ABORT from anyone while ERROR is latched", self.nonce_op(ABORT, 0, b), OK, ST_IDLE)
        print(f"reject: {self.passed} passed, {self.failed} failed")
        return 0 if self.failed == 0 else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--serial", help="exact device serial from list")
    sub = ap.add_subparsers(dest="command", required=True)
    sub.add_parser("list")
    sub.add_parser("info")
    sub.add_parser("status")
    dg = sub.add_parser("diag", help="stack never used since boot and the longest updater pass (M1 #13)")
    dg.add_argument("--clear", action="store_true", help="restart the longest-pass record after reading it")
    ab = sub.add_parser("abort", help="ABORT: clears a latched error from any client, or a session given its nonce")
    ab.add_argument("--nonce", type=lambda s: int(s, 0), default=0)
    rj = sub.add_parser("reject", help="the protocol rejection matrix (M1 #6); needs the chord twice, never commits")
    rj.add_argument("svup", type=Path, help="an image this half accepts")
    up = sub.add_parser("update")
    up.add_argument("svup", type=Path)
    up.add_argument("--delay", type=float, default=0.0, help="seconds between chunks")
    up.add_argument("--rebind-every", type=float, default=50.0, help="renew the client ID and REBIND every S seconds (0: never)")
    up.add_argument("--min-rebinds", type=int, default=0, help="fail before COMMIT unless at least N renewals happened (M1 #16)")
    up.add_argument("--no-commit", action="store_true", help="stop once the image is verified")
    up.add_argument("--keep", action="store_true", help="on failure, leave the session for inspection instead of ABORTing")
    up.add_argument("--halt", choices=sorted(HALT_POINTS, key=HALT_POINTS.get),
                    help="test-hooks builds: stop the commit at this point for good (M1 #8-#10)")
    up.add_argument("--halt-unfed", action="store_true", help="with --halt: let the watchdog reset the board (M1 #9)")
    args = ap.parse_args()

    if args.command == "list":
        print(json.dumps([{k: d.get(k) for k in ("serial_number", "product_string", "vendor_id", "product_id")} for d in devices()], indent=2))
        return 0
    if args.command == "update" and args.halt_unfed and not args.halt:
        ap.error("--halt-unfed needs --halt")
    if args.command == "update" and args.delay >= 30:
        print("warning: --delay of 30 s or more lets the session time out", file=sys.stderr)
    dev = Device(args.serial)
    try:
        if args.command == "info":
            print(json.dumps(decode_info(*dev.op(INFO)), indent=2))
            return 0
        hand = decode_info(*dev.op(INFO))["hand_id"]
        if args.command == "status":
            print(json.dumps(decode_status(*dev.op(STATUS, hand=hand)), indent=2))
            return 0
        if args.command == "diag":
            print(json.dumps(decode_diag(*dev.op(DIAG, bytes([1 if args.clear else 0]), hand=hand)), indent=2))
            return 0
        if args.command == "abort":
            st, r = dev.op(ABORT, struct.pack("<I", args.nonce), hand=hand)
            print(f"ABORT: {status_name(st)}, state {state_name(r[0])}")
            return 0 if st == OK else 1
        if args.command == "reject":
            try:
                return Rejects(dev, args.svup).run()
            except (UpdaterError, ClientIdExpired, RuntimeError) as exc:
                print(f"error: {exc}", file=sys.stderr)
                return 1
        session = Session(dev, args)
        try:
            return session.run(args.svup)
        except (UpdaterError, ClientIdExpired, RuntimeError) as exc:
            print(f"error: {exc}", file=sys.stderr)
            if not args.keep:
                session.abort()
            return 1
        except KeyboardInterrupt:
            if not args.keep:
                session.abort()
            raise
    finally:
        dev.close()


if __name__ == "__main__":
    sys.exit(main())
