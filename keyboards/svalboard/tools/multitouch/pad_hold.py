"""Prototype pad pointer modes on a dev build (SVAL_TOUCH_DEV): show or set
the selectors, and hold their keys on a keyless test board.

usage:
  python pad_hold.py --serial S                      show the selectors
  python pad_hold.py --serial S set --scroll takeover --sniper frame
  python pad_hold.py --serial S hold scroll|sniper|boost [--trigger T] [--seconds N]

`set` changes only what it names. Surface (standard or large) is stored and
applies after the board restarts (`set --surface large --reboot`).

`hold` holds that key on the board (sniper is a /2 hold, boost a x2 hold):
  --trigger scrolllock  (default) while Scroll Lock is on: tap it on, tap it off.
  --trigger x1 | x2     while the mouse's back (x1) or forward (x2) side button
                        is held down. Not a keystroke, so Windows' "after
                        typing" touchpad delay never applies (see below).
The trigger is read every 2 ms; a change goes to the board at once (on and
off), and the hold is repeated every 100 ms. The board lets go by itself
500 ms after the last repeat, only as a safety net if this tool is killed.
Ctrl+C stops (and lets go).

Scroll Lock is a keystroke: with the OS keeping the pads (Scaling, Frame),
Windows' touchpad sensitivity setting can hold back the touchpad for a moment
after any keystroke. Set Settings > Bluetooth & devices > Touchpad >
Taps > Touchpad sensitivity to "Most sensitive" while testing, or use a
mouse side button. The board's own sniper and boost keys send no keystroke.

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
POINT = ["off", "takeover", "scaling", "frame"]
SURFACE = ["standard", "large"]
KEYS = {"scroll": 1, "sniper": 2, "boost": 4}
VK = {"scrolllock": 0x91, "x1": 0x05, "x2": 0x06}
POLL_S = 0.002
REPEAT_S = 0.1


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
    print("supported: " + ", ".join(n for bit, n in ((1, "scroll takeover"), (2, "takeover"), (4, "scaling"), (8, "large surface"), (16, "frame")) if s & bit))
    print(f"grab holders now: 0x{b['holders']:02x}")


class Trigger:
    """The trigger's state, read without a message loop."""

    def __init__(self, name):
        self.user32 = ctypes.windll.user32
        self.vk = VK[name]
        self.toggle = name == "scrolllock"
        # Scroll Lock: its toggle state at start, then flipped on every press
        # seen (GetAsyncKeyState), since a console thread's GetKeyState may lag.
        self.on = bool(self.user32.GetKeyState(self.vk) & 1) if self.toggle else False
        self.down = bool(self.user32.GetAsyncKeyState(self.vk) & 0x8000)

    def read(self):
        down = bool(self.user32.GetAsyncKeyState(self.vk) & 0x8000)
        if self.toggle:
            if down and not self.down:
                self.on = not self.on
        else:
            self.on = down
        self.down = down
        return self.on


def hold(board, args):
    bit = KEYS[args.key]
    if args.seconds is not None:
        end = time.monotonic() + args.seconds
        while time.monotonic() < end:
            command(board, ACTION_KEYS, (bit,))
            time.sleep(REPEAT_S)
        return
    if not sys.platform.startswith("win"):
        raise SystemExit("the triggers need Windows; pass --seconds")
    trig = Trigger(args.trigger)
    what = "Scroll Lock on" if args.trigger == "scrolllock" else f"mouse button {args.trigger} held"
    print(f"{what}: {args.key} held. Ctrl+C quits.", flush=True)
    held, sent_at = False, 0.0
    while True:
        on = trig.read()
        now = time.monotonic()
        if on != held:
            command(board, ACTION_KEYS, (bit if on else 0,))  # at once, both ways
            sent_at = now
            held = on
            print("held" if on else "let go", flush=True)
        elif held and now - sent_at >= REPEAT_S:
            command(board, ACTION_KEYS, (bit,))  # keep it alive
            sent_at = now
        time.sleep(POLL_S)


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
    h.add_argument("--trigger", choices=sorted(VK), default="scrolllock")
    h.add_argument("--seconds", type=float)
    args = p.parse_args()
    if sys.platform.startswith("win"):
        ctypes.windll.winmm.timeBeginPeriod(1)  # 1 ms sleeps for the 2 ms poll
    board = Board(args.serial)
    try:
        b = block(command(board, ACTION_MODES, (0,)))
        if args.cmd == "set":
            new = [b["scroll"], b["sniper"], b["boost"], b["surface_stored"]]
            for i, (v, table) in enumerate(((args.scroll, SCROLL), (args.sniper, POINT), (args.boost, POINT), (args.surface, SURFACE))):
                if v is not None:
                    new[i] = table.index(v)
            show(block(command(board, ACTION_MODES, (1, *new))))
            if args.reboot:
                command(board, ACTION_REBOOT)
                print("restarting")
        elif args.cmd == "hold":
            try:
                hold(board, args)
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
