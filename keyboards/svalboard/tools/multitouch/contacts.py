"""Watch the touch contacts on the native touchpad's pads (Sval 0x2F).

usage: python contacts.py --serial EXACT_SERIAL [--seconds N]

Polls POINTING_CONTACTS (docs/protocol.md, "Pointing contacts") and prints a
line each time the fingers down change: how many on each pad, with their
contact IDs. These are the contacts of the last report the computer took
(what the operating system has), plus any finger beyond the touchpad's 5,
which the board lists too and this tool marks with a *. On exit (after
--seconds, or Ctrl+C) it prints the most fingers that were down at once.

Any build with the native touchpad; the computer must have it in touchpad mode
(Windows does by itself).
"""
import argparse
import sys
import time

from sval_raw import Board

SVAL = 0xDF
OP = 0x2F
POLL_S = 0.01


def parse(payload):
    """The 0x2F payload (after the DF 2F echo) as a dict, or None for no data."""
    if len(payload) < 4 or payload[0] != 1:
        return None
    flags, n = payload[1], payload[3]
    out = dict(reporting=bool(flags & 1), pads=dict(left=bool(flags & 2), right=bool(flags & 4)), left=[], right=[])
    for i in range(n):
        c = payload[4 + 4 * i:8 + 4 * i]
        if len(c) < 4:
            break
        w = int.from_bytes(c[1:4], "little")
        contact = dict(id=c[0] & 15, tip=bool(c[0] & 0x10), confident=bool(c[0] & 0x20), extra=bool(c[0] & 0x80), x=w & 0xFFF, y=(w >> 12) & 0xFFF)
        out["right" if c[0] & 0x40 else "left"].append(contact)
    return out


def down(contacts, side):
    return [c for c in contacts[side] if c["tip"]]


def describe(contacts):
    parts = []
    for side in ("left", "right"):
        fingers = down(contacts, side)
        ids = ",".join(str(c["id"]) + ("*" if c["extra"] else "") for c in fingers)
        parts.append(f"{side} {len(fingers)}" + (f" (IDs {ids})" if fingers else ""))
    total = len(down(contacts, "left")) + len(down(contacts, "right"))
    extra = sum(1 for side in ("left", "right") for c in down(contacts, side) if c["extra"])
    note = f"; {extra} beyond the touchpad's 5 (*)" if extra else ""
    return f"{total} down: " + ", ".join(parts) + note


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--serial", required=True, help="exact USB serial of the board")
    parser.add_argument("--seconds", type=float, help="stop after this long")
    args = parser.parse_args()
    board = Board(args.serial)
    end = time.monotonic() + args.seconds if args.seconds else None
    last, best, best_line, polls = None, -1, "", 0
    try:
        while end is None or time.monotonic() < end:
            try:
                r = board.command(SVAL, OP)
            except RuntimeError:
                # The board dropped this client ID (another app took its
                # place): get a new one and carry on.
                board = Board(args.serial, device=board.dev)
                continue
            polls += 1
            contacts = parse(r[7:])
            if contacts is None:
                line = "no data (no native touchpad, or not the USB half)"
            elif not contacts["reporting"]:
                line = "the touchpad is not reporting (not in touchpad mode)"
            else:
                line = describe(contacts)
                total = len(down(contacts, "left")) + len(down(contacts, "right"))
                if total > best:
                    best, best_line = total, line
            if line != last:
                print(f"{time.strftime('%H:%M:%S')}  {line}", flush=True)
                last = line
            time.sleep(POLL_S)
    except KeyboardInterrupt:
        pass
    finally:
        board.close()
    print(f"{polls} reads; most at once: {best_line or 'none'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
