"""Hold Velocity Scroll on a keyless test board (dev builds with SVAL_TOUCH_DEV).

usage: python velocity_hold.py --serial EXACT_SERIAL [--seconds N]

With no --seconds, Velocity Scroll is held while Scroll Lock is on (Windows):
turn Scroll Lock on, touch a pad and move away from where the finger landed to
scroll; turn it off to let go. With --seconds, it is held that long. The board
lets go by itself 500 ms after this tool stops repeating the hold, so a closed
window never leaves it scrolling. Ctrl+C stops.

Dev builds only: release builds have no raw HID 0xFC command.
"""
import argparse
import ctypes
import sys
import time

from sval_raw import Board

PROTOCOL = 0xFC
OP = 0x42
ACTION_VELOCITY = 2
REPEAT_S = 0.1
VK_SCROLL = 0x91


def hold(board, on):
    r = board.command(PROTOCOL, OP, (ACTION_VELOCITY, 1 if on else 0))
    if r[6] != 0xC2 or not (r[9] & 8):
        raise RuntimeError("this firmware has no Velocity Scroll dev hold (build with SVAL_TOUCH_DEV=yes)")


def scroll_lock_on():
    return bool(ctypes.windll.user32.GetKeyState(VK_SCROLL) & 1)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--serial", required=True, help="exact USB serial of the board")
    parser.add_argument("--seconds", type=float, help="hold this long instead of following Scroll Lock")
    args = parser.parse_args()
    if args.seconds is None and not sys.platform.startswith("win"):
        parser.error("following Scroll Lock needs Windows; pass --seconds")
    board = Board(args.serial)
    held = False
    try:
        if args.seconds is not None:
            end = time.monotonic() + args.seconds
            print(f"holding for {args.seconds:g} s")
            while time.monotonic() < end:
                hold(board, True)
                time.sleep(REPEAT_S)
        else:
            print("Scroll Lock on: Velocity Scroll held. Scroll Lock off: let go. Ctrl+C quits.")
            while True:
                on = scroll_lock_on()
                if on or held:
                    hold(board, on)
                if on != held:
                    print("held" if on else "let go")
                    held = on
                time.sleep(REPEAT_S)
    except KeyboardInterrupt:
        pass
    finally:
        try:
            hold(board, False)
        finally:
            board.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
