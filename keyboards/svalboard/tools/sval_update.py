#!/usr/bin/env python3
"""Svalboard in-firmware updater, host side (M1: the half with USB, over USB).

Needs hidapi (the 'hid' module) and a board running an SVAL_UPDATER=yes build.
Close other configuration clients (Keybard) first: raw HID replies go to every
open handle.

    sval_update.py list
    sval_update.py info
    sval_update.py status
    sval_update.py abort [--nonce N]
    sval_update.py update FILE.svup [--delay S] [--rebind-every S] [--no-commit]

update sends the manifest, prints the manifest hash you confirm, waits for the
chord (Index South + Middle South held for 1 s on this half), then erases,
sends the image, verifies and commits. The client ID is renewed every
--rebind-every seconds (50, as Keybard does) and the session moved to it with
REBIND; after each REBIND the old ID must be refused. --delay S waits S
seconds between chunks (slow-transfer test; the session times out after 30 s
without an op).

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
INFO, MANIFEST, ARM, BEGIN, CHUNK, END, STATUS, COMMIT, ABORT, REBIND = range(10)
MANIFEST_PIECE, CHUNK_PIECE = 20, 18

STATUS_NAMES = ["OK", "INVALID", "UNAVAILABLE", "FLASH_ERR", "ACCEPTED", "BUSY", "NOT_CONFIRMED", "WRONG_HW", "BAD_SIG",
                "BAD_HASH", "BAD_IMAGE", "EPOCH", "OUT_OF_ORDER", "OVERRUN", "TOO_LARGE", "TIMEOUT", "OTHER_CLIENT",
                "UNSUPPORTED"]
STATE_NAMES = ["IDLE", "MANIFEST_LOADING", "CONFIRM_WAIT", "VERIFYING_MANIFEST", "ERASING", "RECEIVING",
               "VERIFYING_IMAGE", "VERIFIED", "COMMITTING", "ERROR"]
OK, ACCEPTED, OUT_OF_ORDER, OTHER_CLIENT, UNSUPPORTED = 0, 4, 12, 16, 17
ST_RECEIVING, ST_VERIFIED, ST_ERROR = 5, 7, 9
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

    def bootstrap(self):
        """A fresh client ID from the Sval client wrapper."""
        nonce = secrets.token_bytes(20)
        r = self._exchange(bytes([WRAPPER]) + bytes(4) + nonce, lambda r: r[0] == WRAPPER and r[1:5] == bytes(4) and r[5:25] == nonce)
        cid = r[25:29]
        if cid in (bytes(4), b"\xFF" * 4):
            raise RuntimeError("Invalid client bootstrap response")
        self.client = cid
        return cid

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
                last_error=status_name(r[21]))


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
        print(f"board: {info['hand']} half, pointing {info['pointing']}, JEDEC {info['jedec']}, state {info['state']}, "
              f"status {info['status']}")
        if m["hand"] != self.hand or m["pointing_id"] != info["pointing_id"]:
            print(f"warning: image is for hand {m['hand']}, pointing {m['pointing_id']}; the board will refuse it")
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
                       signature_ms=s["signature_ms"], verify_ms=s["verify_ms"], erase_ms_max=s["erase_ms_max"])
        print("verified: " + json.dumps(summary))
        if self.args.no_commit:
            print("--no-commit: leaving the verified image staged (ABORT to clear the session)")
            return 0
        st, r = self.nonce_op(COMMIT, struct.pack("<H", crc_body & 0xFFFF))
        if st == UNSUPPORTED:
            print("COMMIT: UNSUPPORTED - this build cannot commit yet; the image is staged and verified")
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


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--serial", help="exact device serial from list")
    sub = ap.add_subparsers(dest="command", required=True)
    sub.add_parser("list")
    sub.add_parser("info")
    sub.add_parser("status")
    ab = sub.add_parser("abort", help="ABORT: clears a latched error from any client, or a session given its nonce")
    ab.add_argument("--nonce", type=lambda s: int(s, 0), default=0)
    up = sub.add_parser("update")
    up.add_argument("svup", type=Path)
    up.add_argument("--delay", type=float, default=0.0, help="seconds between chunks")
    up.add_argument("--rebind-every", type=float, default=50.0, help="renew the client ID and REBIND every S seconds (0: never)")
    up.add_argument("--no-commit", action="store_true", help="stop once the image is verified")
    up.add_argument("--keep", action="store_true", help="on failure, leave the session for inspection instead of ABORTing")
    args = ap.parse_args()

    if args.command == "list":
        print(json.dumps([{k: d.get(k) for k in ("serial_number", "product_string", "vendor_id", "product_id")} for d in devices()], indent=2))
        return 0
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
        if args.command == "abort":
            st, r = dev.op(ABORT, struct.pack("<I", args.nonce), hand=hand)
            print(f"ABORT: {status_name(st)}, state {state_name(r[0])}")
            return 0 if st == OK else 1
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
