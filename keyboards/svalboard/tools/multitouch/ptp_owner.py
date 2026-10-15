"""Get or set who owns the touchpads on a native touchpad dev build (SVAL_TOUCH_DEV).

usage: python ptp_owner.py --serial EXACT_SERIAL {get,mouse,native}

Owners: native (the Precision Touchpad interface; the build default) and
mouse (the pads' plain mouse, as on a build without the touchpad). The
setting is RAM only: power-cycling returns to native. Also prints the Input
Mode, latency and switch values the host last wrote, which hands have a pad,
the touchpad's frame counters and the pad read counters.

Dev builds only: release builds have no raw HID 0xFC command.
"""
import argparse
import struct
import sys

from sval_raw import Board

PROTOCOL = 0xFC
OWNERS = {"mouse": 0, "host": 1, "detector": 2, "native": 3}
NAMES = {v: k for k, v in OWNERS.items()}


def owner_command(board, action, wanted=0):
    r = board.command(PROTOCOL, 0x42, (action, wanted))
    if r[6] != 0xC2:
        raise RuntimeError("this firmware has no touchpad dev command (build with SVAL_TOUCH_DEV=yes)")
    return r


def describe(reply):
    status, owner, built = reply[7], reply[8], reply[9]
    s = reply[10:34]
    native, mode, latency, switches, bits, contacts = s[0], s[1], s[2], s[3], s[4], s[5]
    frames, busy = struct.unpack_from("<II", s, 6)
    scan = struct.unpack_from("<H", s, 14)[0]
    flags = [name for bit, name in ((1, "reporting"), (2, "mouse-hold"), (4, "left-armed"), (8, "right-armed"), (16, "release-pending"), (32, "mouse-suppressed"), (64, "mode3"), (128, "paused for an update")) if bits & bit]
    lines = [
        f"owner: {NAMES.get(owner, owner)}" + (f" (refused, status {status})" if status else ""),
        f"native touchpad built: {'yes' if built & 1 else 'no'}; native owner active: {'yes' if native else 'no'}",
        f"input mode: {mode} ({'touchpad' if mode == 3 else 'mouse'}), latency: {'high' if latency else 'normal'}, "
        f"surface switch: {switches & 1}, button switch: {(switches >> 1) & 1}",
        f"state: {', '.join(flags) or 'idle'}; contacts reported: {contacts}",
        f"frames sent: {frames}, endpoint busy: {busy}, scan time: {scan}",
    ]
    if built & 2:  # status v2
        pads, rejected = s[16], s[17]
        silent, waits, pool_full = struct.unpack_from("<HHH", s, 18)
        hands = [h for bit, h in ((1, "left"), (2, "right")) if pads & bit]
        lines.append(f"pads: {', '.join(hands) or 'none'}; host stopped reading: {silent}; "
                     f"frames held for the other pad: {waits}; pool full: {pool_full}; feature writes refused: {rejected}")
        o = reply[34:]
        samples, errors, read_fail = struct.unpack_from("<III", o, 0)
        retries, recovered, reinits = struct.unpack_from("<HHH", o, 12)
        link = o[18]
        link_text = [t for bit, t in ((1, "hints on"), (2, "no snapshot"), (4, "seen"), (8, "lost")) if link & bit]
        lines.append(f"this pad: {samples} reads, {errors} invalid, {read_fail} read failures, {retries} NACK retries "
                     f"({recovered} recovered), {reinits} re-setups after a chip reset")
        lines.append(f"other pad link: {', '.join(link_text) or 'idle'}")
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--serial", required=True, help="exact USB serial of the board")
    parser.add_argument("action", choices=("get", "mouse", "native"))
    args = parser.parse_args()
    board = Board(args.serial)
    try:
        if args.action == "get":
            reply = owner_command(board, 0)
        else:
            reply = owner_command(board, 1, OWNERS[args.action])
        print(describe(reply))
        return 1 if reply[7] else 0
    finally:
        board.close()


if __name__ == "__main__":
    sys.exit(main())
