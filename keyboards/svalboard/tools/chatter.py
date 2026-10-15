#!/usr/bin/env python3
"""Raw matrix chatter check for a Svalboard dev build with SVAL_CHATTER_LOG=yes.

Clears the chatter counters on both halves, waits (type normally, or leave the
board alone for an idle test), then prints every key that changed and flags
"fast" transitions: a press or release shorter than 3 ms, or lasting a
single scan frame, faster than any finger. Exit status 1 when any key
chattered, 2 when the board has no log.

  python chatter.py --serial sval:XXXX --seconds 120

Reads through the Scan Lab channel (kb/scanlab.h, SCANLAB_OP_CHATTER_*).
"""
import argparse
import json
import os
import sys
import time

REPORT = 64
VIA_SET, VIA_GET, CHANNEL = 0x07, 0x08, 0x53
OP_CLEAR, OP_ROW, OP_GAP = 0x06, 0x80, 0xA0
HANDS = ("left", "right")


def hid_backend():
    if sys.platform.startswith("linux"):
        import hidraw
        return hidraw
    import hid
    return hid


def labels():
    # kb/info.json: matrix [row, col] -> label (l1s, rtdd, ...). Rows 0-4 left, 5-9 right.
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "info.json")
    out = {}
    try:
        with open(path) as f:
            for k in json.load(f)["layouts"]["LAYOUT"]["layout"]:
                out[tuple(k["matrix"])] = k["label"]
    except (OSError, KeyError, ValueError):
        pass
    return out


class Board:
    def __init__(self, serial):
        hid = hid_backend()
        found = [d for d in hid.enumerate() if d.get("serial_number") == serial and d.get("usage_page") == 0xFF61 and d.get("usage") == 0x62]
        if len(found) != 1:
            sys.exit(f"No Svalboard with serial {serial} (found {len(found)})")
        self.h = hid.device()
        self.h.open_path(found[0]["path"])

    def tx(self, packet):
        packet = bytes(packet).ljust(REPORT, b"\0")
        self.h.write(b"\0" + packet)
        deadline = time.monotonic() + 2.0
        while time.monotonic() < deadline:
            r = bytes(self.h.read(REPORT, 200))
            if len(r) >= 3 and r[0] == packet[0] and r[1] == CHANNEL and r[2] == packet[2]:
                return r[3:]
        sys.exit("No reply from the board")

    def clear(self, hand):
        return self.tx([VIA_SET, CHANNEL, OP_CLEAR, hand])[0] == 1

    def row(self, op, hand, row):
        r = self.tx([VIA_GET, CHANNEL, op | (hand << 3) | row])
        if r[0] == 0xFF and not any(r[1:23]):
            return None  # the other half did not answer
        if r[18] != 1:
            return None
        words = [int.from_bytes(r[2 * c:2 * c + 2], "little") for c in range(6)]
        return words, list(r[12:18]), int.from_bytes(r[19:23], "little")


def read_all(b, hands):
    out = {}
    for hand in hands:
        for row in range(5):
            a = b.row(OP_ROW, hand, row)
            g = b.row(OP_GAP, hand, row)
            if a is None or g is None:
                break
            out[(hand, row)] = (a, g)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--serial", required=True, help="exact serial (sval_update.py list)")
    ap.add_argument("--seconds", type=float, default=60)
    ap.add_argument("--no-clear", action="store_true", help="read the counters as they are")
    args = ap.parse_args()

    b = Board(args.serial)
    names = labels()
    hands = []
    for hand in (0, 1):
        if args.no_clear or b.clear(hand):
            hands.append(hand)
    if not hands or not read_all(b, hands):
        print("This build has no chatter log (build with -e SVAL_CHATTER_LOG=yes).")
        return 2
    print(f"Logging {', '.join(HANDS[h] for h in hands)} for {args.seconds:g} s. Ctrl+C stops early.")
    try:
        end = time.monotonic() + args.seconds
        while time.monotonic() < end:
            time.sleep(min(1.0, max(0.0, end - time.monotonic())))
    except KeyboardInterrupt:
        pass

    data = read_all(b, hands)
    total = fast_total = 0
    rows = []
    for (hand, row), ((trans, fast, ms), (gap, seen, _)) in sorted(data.items()):
        for c in range(6):
            if not trans[c]:
                continue
            total += trans[c]
            fast_total += fast[c]
            name = names.get((row + 5 * hand, c), f"r{row}c{c}")
            g = f"{gap[c] / 1000:.2f} ms" if seen[c] and gap[c] != 0xFFFF else "-"
            rows.append((fast[c], f"{HANDS[hand]:5} {name:5} r{row}c{c}  {trans[c]:6}  {fast[c]:5}  {g:>10}"))
    secs = max((v[0][2] for v in data.values()), default=0) / 1000
    print(f"\n{secs:.0f} s logged, {total} transitions, {fast_total} fast (under 3 ms, or one frame)\n")
    if rows:
        print("hand  key   pos   trans   fast   shortest")
        for _, line in sorted(rows, key=lambda x: -x[0]):
            print(line)
    print("\nRESULT:", "CHATTER" if fast_total else "clean")
    return 1 if fast_total else 0


if __name__ == "__main__":
    sys.exit(main())
