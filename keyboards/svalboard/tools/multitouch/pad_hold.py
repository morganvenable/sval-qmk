"""Prototype pad pointer modes on a dev build (SVAL_TOUCH_DEV): show or set
the selectors, and hold their keys on a keyless test board.

usage:
  python pad_hold.py --serial S                      show the selectors
  python pad_hold.py --serial S set --scroll takeover --sniper scaling
  python pad_hold.py --serial S hold scroll|sniper|boost [--seconds N]

`set` changes only what it names. Surface (standard or large) is stored and
applies after the board restarts (`set --surface large --reboot`).

`hold` keeps that key held while Scroll Lock is on (Windows), like the
Velocity Scroll tool: sniper is a /2 hold, boost a x2 hold. The board lets go
by itself 500 ms after the tool stops repeating. Ctrl+C stops.

Dev builds only: release builds have no raw HID 0xFC command.
"""
import argparse
import ctypes
import sys
import time

from sval_raw import Board

PROTOCOL, OP = 0xFC, 0x42
ACTION_MODES, ACTION_KEYS, ACTION_REBOOT = 3, 4, 5
SCROLL = ["off", "takeover"]
POINT = ["off", "takeover", "scaling"]
SURFACE = ["standard", "large"]
KEYS = {"scroll": 1, "sniper": 2, "boost": 4}
VK_SCROLL = 0x91


def command(board, action, args=()):
    r = board.command(PROTOCOL, OP, (action, *args))
    if r[6] != 0xC2 or not (r[9] & 16):
        raise RuntimeError("this firmware has no pad pointer modes (dev build with SVAL_TOUCH_DEV=yes)")
    if r[7] != 0:
        raise RuntimeError(f"refused (status {r[7]})")
    return r


def block(r):
    b = r[56:64]
    return {
        "version": b[0], "scroll": b[1], "sniper": b[2], "boost": b[3],
        "surface_stored": b[4], "surface_active": b[5], "supported": b[6], "holders": b[7],
    }


def show(b):
    name = lambda table, v: table[v] if v < len(table) else f"?{v}"
    print(f"scroll  {name(SCROLL, b['scroll'])}")
    print(f"sniper  {name(POINT, b['sniper'])}")
    print(f"boost   {name(POINT, b['boost'])}")
    pending = "" if b["surface_stored"] == b["surface_active"] else f" (stored: {name(SURFACE, b['surface_stored'])}, after restart)"
    print(f"surface {name(SURFACE, b['surface_active'])}{pending}")
    s = b["supported"]
    print("supported: " + ", ".join(n for bit, n in ((1, "scroll takeover"), (2, "takeover"), (4, "scaling"), (8, "large surface")) if s & bit))
    print(f"grab holders now: 0x{b['holders']:02x}")


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--serial", required=True, help="exact USB serial of the board")
    sub = p.add_subparsers(dest="cmd")
    s = sub.add_parser("set")
    s.add_argument("--scroll", choices=SCROLL)
    s.add_argument("--sniper", choices=POINT)
    s.add_argument("--boost", choices=POINT)
    s.add_argument("--surface", choices=SURFACE)
    s.add_argument("--reboot", action="store_true", help="restart the board afterwards")
    h = sub.add_parser("hold")
    h.add_argument("key", choices=sorted(KEYS))
    h.add_argument("--seconds", type=float)
    args = p.parse_args()
    board = Board(args.serial)
    try:
        b = block(command(board, ACTION_MODES, (0,)))
        if args.cmd == "set":
            new = [b["scroll"], b["sniper"], b["boost"], b["surface_stored"]]
            for i, (v, table) in enumerate(((args.scroll, SCROLL), (args.sniper, POINT), (args.boost, POINT), (args.surface, SURFACE))):
                if v is not None:
                    new[i] = table.index(v)
            b = block(command(board, ACTION_MODES, (1, *new)))
            show(b)
            if args.reboot:
                command(board, ACTION_REBOOT)
                print("restarting")
        elif args.cmd == "hold":
            if args.seconds is None and not sys.platform.startswith("win"):
                p.error("following Scroll Lock needs Windows; pass --seconds")
            bit, held = KEYS[args.key], False
            try:
                if args.seconds is not None:
                    end = time.monotonic() + args.seconds
                    while time.monotonic() < end:
                        command(board, ACTION_KEYS, (bit,))
                        time.sleep(0.1)
                else:
                    print(f"Scroll Lock on: {args.key} held. Scroll Lock off: let go. Ctrl+C quits.")
                    while True:
                        on = bool(ctypes.windll.user32.GetKeyState(VK_SCROLL) & 1)
                        if on or held:
                            command(board, ACTION_KEYS, (bit if on else 0,))
                        if on != held:
                            print("held" if on else "let go")
                            held = on
                        time.sleep(0.1)
            except KeyboardInterrupt:
                pass
            finally:
                command(board, ACTION_KEYS, (0,))
        else:
            show(b)
    finally:
        board.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
