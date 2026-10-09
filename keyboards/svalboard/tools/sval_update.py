#!/usr/bin/env python3
"""Svalboard in-firmware updater, host side: the half with USB over USB (M1),
and the other half through it over the split link (M2).

Needs hidapi (the 'hid' module) and a board running an SVAL_UPDATER=yes build.
Close other configuration clients (Keybard) first: raw HID replies go to every
open handle.

    sval_update.py list
    sval_update.py info [--other]
    sval_update.py status
    sval_update.py relay
    sval_update.py diag [--clear]
    sval_update.py abort [--nonce N]
    sval_update.py update FILE.svup [--delay S] [--rebind-every S] [--min-rebinds N]
                                    [--no-commit] [--halt POINT [--halt-unfed]]
    sval_update.py pair FILE1.svup FILE2.svup [--delay S] [--rebind-every S]
    sval_update.py reject FILE.svup

update sends the manifest, prints the manifest hash you confirm, waits for the
chord (Index South + Middle South held for 1 s on the half with USB), then
erases, sends the image, verifies and commits.

The image's own hand picks the half. An image for the other half (the one
without USB) is staged and verified on the half with USB, then relayed to the
other half, which checks the signature and hash itself (D7); a second COMMIT
makes it write the image and reset. The link to it pauses while it erases,
verifies and commits (both halves type while the image streams). The tool
then waits up to 10 s for the other half to answer with the new version. With
only one half updated the halves differ, which is not supported (V): the half
with USB blinks red until both match. pair does both: the other half first,
then the half with USB over USB (the chord twice, once per image), then waits
for the board to come back and for presence to show the same release on both
halves within 10 s. relay prints the relay's progress; info --other asks the
other half. The client ID is renewed every
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
INFO, MANIFEST, ARM, BEGIN, CHUNK, END, STATUS, COMMIT, ABORT, REBIND, TEST_HALT, DIAG, RELAY = range(13)
HALT_POINTS = {"invalidated": 1, "first-erase": 2, "mid-program": 3, "last-sector": 4, "page0": 5, "fault": 6,
               "fault-erased": 7}
MANIFEST_PIECE, CHUNK_PIECE = 20, 18

STATUS_NAMES = ["OK", "INVALID", "UNAVAILABLE", "FLASH_ERR", "ACCEPTED", "BUSY", "NOT_CONFIRMED", "WRONG_HW", "BAD_SIG",
                "BAD_HASH", "BAD_IMAGE", "EPOCH", "OUT_OF_ORDER", "OVERRUN", "TOO_LARGE", "TIMEOUT", "OTHER_CLIENT",
                "UNSUPPORTED", "STORAGE"]
STATE_NAMES = ["IDLE", "MANIFEST_LOADING", "CONFIRM_WAIT", "VERIFYING_MANIFEST", "ERASING", "RECEIVING",
               "VERIFYING_IMAGE", "VERIFIED", "COMMITTING", "ERROR", "RELAYING", "RELAYED", "SUBSIDE_COMMITTING"]
OK, INVALID, ACCEPTED, NOT_CONFIRMED, BAD_HASH = 0, 1, 4, 6, 9
OUT_OF_ORDER, OVERRUN, OTHER_CLIENT, UNSUPPORTED = 12, 13, 16, 17
ST_IDLE, ST_MANIFEST_LOADING, ST_CONFIRM_WAIT, ST_RECEIVING, ST_VERIFIED, ST_ERROR = 0, 1, 2, 5, 7, 9
ST_RELAYING, ST_RELAYED, ST_SUBSIDE_COMMITTING = 10, 11, 12
# relay_phase_t (keyboards/svalboard/updater/update_split.h)
RELAY_PHASES = ["idle", "start", "manifest", "checking and erasing", "sending", "end", "verifying", "verified",
                "commit", "committing", "probe", "done", "failed", "aborting"]
PRESENCE_NONE, PRESENCE_MATCH, PRESENCE_VERSION = 0, 1, 2
HANDS = {0: "left", 1: "right"}
POINTING = {v: k for k, v in make_update.POINTING.items()}


def status_name(code):
    return STATUS_NAMES[code] if code < len(STATUS_NAMES) else f"status {code}"


# The other half by split presence (update_presence_status_t; M2, V).
PRESENCE_NAMES = ["none", "match", "version differs", "same hand", "invalid answer"]


def presence_name(code):
    return PRESENCE_NAMES[code] if code < len(PRESENCE_NAMES) else f"presence {code}"


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
                test_key=bool(r[20] & 16), other_half_mismatch=bool(r[20] & 32), last_error=status_name(r[21]))


def image_refusal(m, info):
    """Why a half would refuse this manifest, as far as its INFO shows (None: no reason seen).
    The same order of checks as update_manifest_check() (update_image.c)."""
    if m["key_id"] == 0 and (m["flags"] & make_update.FLAG_RELEASE or info["release_build"] or not info["test_key"]):
        return "BAD_SIG: signed with the TEST-ONLY key, which this build does not accept"
    if info["release_build"] and m["flags"] & make_update.FLAG_DIAGNOSTIC:
        return "BAD_IMAGE: a DIAGNOSTIC image on a release build"
    if m["hand"] != info["hand_id"]:
        return f"UNSUPPORTED: the image is for hand {HANDS.get(m['hand'], m['hand'])}, the half is {info['hand']}"
    if m["pointing_id"] != info["pointing_id"]:
        return f"WRONG_HW: the image is for pointing {POINTING.get(m['pointing_id'], m['pointing_id'])}, the half has {info['pointing']}"
    if m["security_epoch"] < info["security_epoch"]:
        return f"EPOCH: image epoch {m['security_epoch']}, the half's floor {info['security_epoch']}"
    if m["storage_format"] < info["storage_format"]:
        return f"STORAGE: image storage format {m['storage_format']}, the half's floor {info['storage_format']}"
    if m["image_len"] > int(info["max_image"], 16):
        return f"TOO_LARGE: {m['image_len']} B, the half takes at most {info['max_image']}"
    return None


def fw_differs(seen, m):
    """A half reports another fw than the manifest it was given (both set: a build with
    EXTRAFLAGS=-DSVAL_FW_VERSION=N and an image made with --fw-version N)."""
    return seen != m["fw_version"] and seen != 0 and m["fw_version"] != 0


FW_NOTE = ("(until M3 a build reports the SVAL_FW_VERSION it was compiled with, which make_update.py does not set: "
           "build with EXTRAFLAGS=-DSVAL_FW_VERSION=N and make the image with --fw-version N)")


def relay_phase_name(code):
    return RELAY_PHASES[code] if code < len(RELAY_PHASES) else f"phase {code}"


def decode_relay(st, r):
    """The RELAY op: the relay to the other half (M2)."""
    return dict(status=status_name(st), phase=relay_phase_name(r[0]), phase_id=r[0], other_state=state_name(r[1]),
                other_state_id=r[1], other_error=status_name(r[2]), other_error_id=r[2],
                acked=r[3] | r[4] << 8 | r[5] << 16, image_len=r[6] | r[7] << 8 | r[8] << 16,
                other_sectors_erased=struct.unpack_from("<H", r, 9)[0],
                other_sectors_to_erase=struct.unpack_from("<H", r, 11)[0], retries=struct.unpack_from("<H", r, 13)[0],
                relay_ms=struct.unpack_from("<I", r, 15)[0], link_paused=bool(r[19] & 1), unconfirmed=bool(r[19] & 2),
                error=status_name(r[20]),
                error_id=r[20], state=state_name(r[21]), state_id=r[21])


def decode_diag(st, r):
    return dict(status=status_name(st), stack_unused=struct.unpack_from("<H", r, 0)[0],
                stack_size=struct.unpack_from("<H", r, 2)[0], longest_pass_ms=struct.unpack_from("<H", r, 4)[0],
                longest_pass_state=state_name(r[6]), **decode_crumbs(r))


OP_NAMES = ["INFO", "MANIFEST", "ARM", "BEGIN", "CHUNK", "END", "STATUS", "COMMIT", "ABORT", "REBIND", "TEST_HALT", "DIAG",
            "RELAY"]


def decode_crumbs(r):
    """Why the chip last reset, and what the updater was doing just before (watchdog scratch 0-3)."""
    if len(r) < 20:
        return {}
    f = r[7]
    why = [n for b, n in ((1, "power-on/brown-out"), (2, "RUN pin"), (4, "debugger"), (8, "watchdog timer"),
                          (16, "watchdog force")) if f & b] or ["none of POR/RUN/watchdog: software or core reset"]
    out = dict(last_reset=why)
    if not f & 0x80:
        out["before_reset"] = "no breadcrumbs (first boot, power-on or RUN pin)"
        return out
    c1, c2, c3 = struct.unpack_from("<III", r, 8)
    op, state, st = c1 >> 24, (c1 >> 16) & 0xFF, (c1 >> 8) & 0xFF
    out["before_reset"] = dict(
        last_op=OP_NAMES[op] if op < len(OP_NAMES) else f"op {op}", state=state_name(state),
        op_status="still inside the op" if st == 0xFF else status_name(st), op_count=c1 & 0xFF,
        last_op_ms=c2, last_pass_ms=c3)
    return out


def decode_status(st, r):
    return dict(status=status_name(st), state=state_name(r[0]), state_id=r[0], staged=r[1] | r[2] << 8 | r[3] << 16,
                image_len=r[4] | r[5] << 8 | r[6] << 16, sectors_erased=struct.unpack_from("<H", r, 7)[0],
                sectors_to_erase=struct.unpack_from("<H", r, 9)[0], last_error=status_name(r[11]), last_error_id=r[11],
                signature_ms=struct.unpack_from("<H", r, 12)[0], verify_ms=struct.unpack_from("<H", r, 14)[0],
                erase_ms_max=struct.unpack_from("<H", r, 16)[0], crc_lo16=struct.unpack_from("<H", r, 18)[0],
                chord_made=bool(r[20] & 1), session_bound=bool(r[20] & 2), settings_writes_failing=bool(r[20] & 4),
                other_half=presence_name(r[21]), other_half_id=r[21])


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

    def run(self, path, confirm_other=True):
        data = Path(path).read_bytes()
        manifest, sig, image = make_update.parse_svup(data)
        self.image = image
        m = make_update.parse_manifest(manifest)
        mhash = hashlib.sha512(manifest).digest()[:4]
        crc_body = make_update.crc32_mpeg2(image[0x100:])
        version = m["version"].rstrip(b"\0").decode("ascii", "replace")

        info = decode_info(*self.dev.op(INFO))
        board_hand = info["hand_id"]
        # The image's own hand picks the half: this one, or the other one through it (M2).
        self.other = m["hand"] in HANDS and m["hand"] == board_hand ^ 1
        self.hand = m["hand"] if self.other else board_hand
        if getattr(self.args, "halt", None) and not info["test_hooks"]:
            raise UpdaterError("--halt needs an SVAL_UPDATE_TEST_HOOKS build on the board")
        if getattr(self.args, "halt", None) and self.other:
            raise UpdaterError("--halt is for the half with USB only")
        print(f"board: {info['hand']} half, pointing {info['pointing']}, JEDEC {info['jedec']}, state {info['state']}, "
              f"status {info['status']}")
        target = self.other_half(m) if self.other else info
        if not self.other and (m["hand"] != self.hand or m["pointing_id"] != info["pointing_id"]):
            print(f"warning: image is for hand {m['hand']}, pointing {m['pointing_id']}; the board will refuse it")
        if m["key_id"] == 0 and not target["test_key"]:
            print("warning: image is signed with the TEST-ONLY key, which this build does not accept "
                  "(build with SVAL_UPDATE_TEST_KEY=yes); the board will refuse it")
        print(f"image: {version!r} ({m['fw_version']}), {len(image)} B, key_id {m['key_id']}, flags {m['flags']:#x}"
              + (f", for the other ({HANDS[self.hand]}) half" if self.other else ""))

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
        usb_half = info["hand"]
        print()
        print(f"  MANIFEST HASH  {mhash.hex()}   ({version}, {len(image)} bytes"
              + (f", for the {HANDS[self.hand]} half without USB)" if self.other else ")"))
        print(f"  Check this matches what you expect, then confirm on the keyboard: hold Index South + Middle South")
        print(f"  for 1 second on the {usb_half} half, the one with USB (M and , on the right; C and V on the left).")
        print("  The LED blinks blue.")
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
        if self.other:
            return self.relay_and_commit(crc_body, m, confirm_other)
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

    # ---- the other half (M2) ----

    def wait_presence(self, timeout):
        """STATUS until split presence shows the other half (or timeout); returns the last STATUS."""
        deadline = time.monotonic() + timeout
        while True:
            s = decode_status(*self.dev.op(STATUS, hand=self.hand))
            if s["other_half_id"] != 0 or time.monotonic() >= deadline:
                return s
            time.sleep(0.25)

    def other_half(self, m):
        """The half without USB must answer presence as the same release or another version, and INFO."""
        s = self.wait_presence(5)
        if s["other_half_id"] not in (PRESENCE_MATCH, PRESENCE_VERSION):
            raise UpdaterError(f"the other half: presence {s['other_half']}; it must be connected and run an "
                               "SVAL_UPDATER build (if it does not answer, plug USB into it and copy the .uf2)")
        st, r = self.dev.op(INFO, hand=self.hand)
        if r[0] == 0xFF:
            raise UpdaterError(f"the other half does not answer INFO ({status_name(st)})")
        oi = decode_info(st, r)
        print(f"other half: {oi['hand']}, pointing {oi['pointing']}, fw {oi['fw_version']}, state {oi['state']}, "
              f"status {oi['status']}, presence {s['other_half']}")
        if m["pointing_id"] != oi["pointing_id"]:
            print(f"warning: image is for pointing {m['pointing_id']}, the other half has {oi['pointing_id']}; "
                  "it will be refused")
        return oi

    def relay(self):
        return decode_relay(*self.dev.op(RELAY, hand=self.hand))

    def relay_and_commit(self, crc_body, m, confirm_other):
        crc = struct.pack("<H", crc_body & 0xFFFF)
        self.need("COMMIT (relay)", self.nonce_op(COMMIT, crc), ACCEPTED)
        print("relaying to the other half, which checks the signature and the hash itself")
        deadline = time.monotonic() + 180
        last = None
        while True:
            self.maybe_renew()
            rl = self.relay()
            if rl["state_id"] == ST_ERROR:
                s = self.status()
                raise UpdaterError(f"relay: {s['last_error']} (relay {rl['phase']}, {rl['error']}; other half "
                                   f"{rl['other_state']}, {rl['other_error']})")
            if rl["state_id"] == ST_RELAYED:
                break
            if rl["state_id"] != ST_RELAYING:
                raise UpdaterError(f"relay: unexpected state {rl['state']}")
            if rl["phase_id"] == 4:
                now = f"sending {rl['acked'] * 100 // max(rl['image_len'], 1) // 5 * 5}%"
            elif rl["phase_id"] == 3 and rl["other_sectors_to_erase"]:
                now = f"the other half erases ({rl['other_sectors_erased']}/{rl['other_sectors_to_erase']} sectors)"
            elif rl["phase_id"] == 3:
                now = "the other half checks the signature"
            else:
                now = rl["phase"]
            if now != last:
                print(f"  {now}")
                last = now
            if time.monotonic() > deadline:
                raise UpdaterError(f"relay: no end in 180 s ({rl['phase']})")
            time.sleep(0.2)
        print("relayed: " + json.dumps(dict(relay_ms=rl["relay_ms"], retries=rl["retries"], bytes=rl["acked"])))
        # No REBIND from here: in SUBSIDE_COMMITTING only INFO, STATUS and RELAY are answered.
        self.need("COMMIT (other half)", self.nonce_op(COMMIT, crc), ACCEPTED)
        print("the other half writes the image and resets; the split link stays paused meanwhile")
        deadline = time.monotonic() + 30
        while True:
            s = self.status()
            if s["state_id"] == ST_ERROR:
                rl = self.relay()
                raise UpdaterError(f"the other half's commit: {s['last_error']} (other half {rl['other_state']}, "
                                   f"{rl['other_error']})")
            if s["state_id"] == ST_IDLE:
                break
            if time.monotonic() > deadline:
                raise UpdaterError(f"the other half's commit: no end in 30 s ({s['state']})")
            time.sleep(0.2)
        rl = self.relay()
        if rl["unconfirmed"]:
            # Expected: the hold usually ends before the other half has rebooted.
            print("the other half took COMMIT (not yet confirmed: it was still rebooting); checking its version")
        else:
            print("the other half took COMMIT")
        self.nonce = None
        if not confirm_other:
            return 0
        return self.confirm_other(m)

    def confirm_other(self, m):
        """Up to 10 s (after its reboot) for the other half to answer again; its version and presence."""
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            st, r = self.dev.op(INFO, hand=self.hand)
            if r[0] != 0xFF:
                seen = decode_info(st, r)["fw_version"]
                s = decode_status(*self.dev.op(STATUS, hand=self.hand))
                # Presence reads NONE until its first ping after the reboot.
                while s["other_half_id"] == PRESENCE_NONE and time.monotonic() < deadline:
                    time.sleep(0.25)
                    s = decode_status(*self.dev.op(STATUS, hand=self.hand))
                print(f"the other half answers again: fw {seen}, presence {s['other_half']}")
                if fw_differs(seen, m):
                    raise UpdaterError(f"the other half reports fw {seen}, the image's manifest says fw "
                                       f"{m['fw_version']}: the update did not take")
                if seen != m["fw_version"]:
                    print(f"warning: the image's manifest says fw {m['fw_version']} {FW_NOTE}")
                if s["other_half_id"] != PRESENCE_MATCH:
                    print("note: the halves now run different releases, which is not supported (V): the half "
                          "with USB blinks red until it is updated too (use pair)")
                return 0
            time.sleep(0.5)
        print("warning: the other half did not answer within 10 s. A new release may only talk to a matching half: "
              "update the half with USB too (pair). If it still does not answer, plug USB into it and copy the .uf2.")
        return 4

    def abort(self):
        if self.nonce is not None:
            try:
                st, _ = self.nonce_op(ABORT)
                print(f"ABORT: {status_name(st)}", file=sys.stderr)
            except Exception as exc:  # the board may be gone
                print(f"ABORT failed: {exc}", file=sys.stderr)


def reopen(serial, timeout):
    """The board again after it reset (USB comes back with the new firmware)."""
    deadline = time.monotonic() + timeout
    while True:
        try:
            return Device(serial)
        except (RuntimeError, OSError, IOError):
            if time.monotonic() >= deadline:
                raise
            time.sleep(0.5)


def pair(dev, args, paths, sessions):
    """Both halves: the other one through this one, then this one over USB; then presence must match (V)."""
    images = []
    for p in paths:
        manifest, _, _ = make_update.parse_svup(Path(p).read_bytes())
        images.append((Path(p), make_update.parse_manifest(manifest)))
    board = decode_info(*dev.op(INFO))["hand_id"]
    other = [(p, m) for p, m in images if m["hand"] == board ^ 1]
    this = [(p, m) for p, m in images if m["hand"] == board]
    if len(other) != 1 or len(this) != 1:
        raise UpdaterError(f"pair needs one image for each half; this half is {HANDS.get(board, board)}, the images are for "
                           + ", ".join(HANDS.get(m["hand"], str(m["hand"])) for _, m in images))
    # Both images are checked against both halves before anything is sent:
    # a refusal of the second image after the first half committed would
    # leave the halves on different releases.
    deadline = time.monotonic() + 5
    while True:
        s = decode_status(*dev.op(STATUS, hand=board))
        if s["other_half_id"] in (PRESENCE_MATCH, PRESENCE_VERSION) or time.monotonic() >= deadline:
            break
        time.sleep(0.25)
    if s["other_half_id"] not in (PRESENCE_MATCH, PRESENCE_VERSION):
        raise UpdaterError(f"pair: the other half: presence {s['other_half']}; it must be connected and run an "
                           "SVAL_UPDATER build")
    st, r = dev.op(INFO, hand=board ^ 1)
    if r[0] == 0xFF:
        raise UpdaterError(f"pair: the other half does not answer INFO ({status_name(st)})")
    infos = {board: decode_info(*dev.op(INFO)), board ^ 1: decode_info(st, r)}
    for p, m in other + this:
        why = image_refusal(m, infos[m["hand"]])
        if why:
            raise UpdaterError(f"pair: {p.name} would be refused by the {HANDS[m['hand']]} half ({why}); nothing was sent")
    print("pair: the chord is made twice, both times on this half (the one with USB): once for each image")
    print(f"== 1/2: the other ({HANDS[board ^ 1]}) half, {other[0][0].name} ==")
    sessions.append(Session(dev, args))
    rc = sessions[-1].run(other[0][0], confirm_other=False)
    if rc != 0:
        return rc
    # Straight on, without waiting to hear from the other half: it may not be
    # able to talk to this half's old firmware (V).
    print(f"== 2/2: this ({HANDS[board]}) half, {this[0][0].name} ==")
    sessions.append(Session(dev, args))
    rc = sessions[-1].run(this[0][0])
    if rc != 0:
        return rc
    serial = dev.serial
    dev.close()
    print("waiting for the board to come back")
    dev2 = reopen(serial, 30)
    try:
        t0 = time.monotonic()
        deadline = t0 + 10
        s = None
        while time.monotonic() < deadline:
            try:
                s = decode_status(*dev2.op(STATUS, hand=board))
            except (RuntimeError, OSError, IOError):
                # Opened while the board was still resetting: open it again.
                dev2.close()
                dev2 = reopen(serial, max(1, deadline - time.monotonic()))
                continue
            if s["other_half_id"] == PRESENCE_MATCH:
                break
            time.sleep(0.5)
        if s is None or s["other_half_id"] != PRESENCE_MATCH:
            raise UpdaterError(f"presence did not show the same release on both halves within 10 s "
                               f"({s['other_half'] if s else 'no STATUS'}); if the other half does not answer, "
                               "plug USB into it and copy the .uf2")
        mine = decode_info(*dev2.op(INFO))["fw_version"]
        st, r = dev2.op(INFO, hand=board ^ 1)
        theirs = decode_info(st, r)["fw_version"] if r[0] != 0xFF else None
        print(f"pair: presence {s['other_half']} after {time.monotonic() - t0:.1f} s; this half fw {mine}, "
              f"the other half fw {theirs}")
        if (theirs is None or fw_differs(mine, this[0][1]) or fw_differs(theirs, other[0][1])):
            raise UpdaterError(f"pair: the manifests say fw {this[0][1]['fw_version']} (this half) and "
                               f"{other[0][1]['fw_version']} (the other half), the halves report {mine} and {theirs}")
        if mine != this[0][1]["fw_version"] or theirs != other[0][1]["fw_version"]:
            print(f"warning: the manifests say fw {this[0][1]['fw_version']} and {other[0][1]['fw_version']} {FW_NOTE}")
        return 0
    finally:
        dev2.close()


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
    inf = sub.add_parser("info")
    inf.add_argument("--other", action="store_true", help="the other half, over the split link (M2)")
    sub.add_parser("status")
    sub.add_parser("relay", help="the relay to the other half: phase and progress (M2)")
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
    pr = sub.add_parser("pair", help="update both halves: the other one through this one, then this one (M2)")
    pr.add_argument("svup", type=Path, nargs=2, help="one image for each half, in any order")
    pr.add_argument("--delay", type=float, default=0.0, help="seconds between chunks")
    pr.add_argument("--rebind-every", type=float, default=50.0, help="renew the client ID and REBIND every S seconds (0: never)")
    pr.add_argument("--keep", action="store_true", help="on failure, leave the session for inspection instead of ABORTing")
    args = ap.parse_args()

    if args.command == "list":
        print(json.dumps([{k: d.get(k) for k in ("serial_number", "product_string", "vendor_id", "product_id")} for d in devices()], indent=2))
        return 0
    if args.command == "pair":
        args.min_rebinds, args.no_commit, args.halt, args.halt_unfed = 0, False, None, False
    if args.command == "update" and args.halt_unfed and not args.halt:
        ap.error("--halt-unfed needs --halt")
    if args.command == "update" and args.delay >= 30:
        print("warning: --delay of 30 s or more lets the session time out", file=sys.stderr)
    dev = Device(args.serial)
    try:
        if args.command == "info" and not args.other:
            print(json.dumps(decode_info(*dev.op(INFO)), indent=2))
            return 0
        hand = decode_info(*dev.op(INFO))["hand_id"]
        if args.command == "info":
            st, r = dev.op(INFO, hand=hand ^ 1)
            print(json.dumps(decode_info(st, r) if r[0] != 0xFF else dict(status=status_name(st), answer="none"), indent=2))
            return 0 if st == OK else 1
        if args.command == "relay":
            print(json.dumps(decode_relay(*dev.op(RELAY, hand=hand)), indent=2))
            return 0
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
        if args.command == "pair":
            sessions = []
            try:
                return pair(dev, args, args.svup, sessions)
            except (UpdaterError, ClientIdExpired, RuntimeError) as exc:
                print(f"error: {exc}", file=sys.stderr)
                if sessions and not args.keep:
                    sessions[-1].abort()
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
