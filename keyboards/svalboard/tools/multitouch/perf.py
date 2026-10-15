"""Read the timing counters of a SVAL_PERF_STATS build (raw HID 0xFC op 0x43).

usage: python perf.py --serial EXACT_SERIAL {get,reset,watch,run} [SECONDS]

  get          counters since the last reset, both halves
  reset        zero the counters on both halves
  run SECONDS  reset, wait, then print the window's numbers once
  watch [N]    reset, then print one line per second for N seconds (default 10)

Rates are per second over the window: matrix frames (scans), main-loop
passes, pad I2C reads, split RPCs (the other half's pad), touchpad frames.
Maximums are the worst single gap or read in the window. The test board has
no keys; on a real board each scan adds the row reads (about 0.9 ms).
"""
import argparse
import secrets
import struct
import sys
import time

from capture import open_device

PROTOCOL = 0xFC
REFUSED = {"sval:E465B0621736062B": "the daily driver",
           "sval:E465B0621725122B": "the Trackball Right Mule"}
BUCKETS = ("<1", "1-2", "2-4", "4-8", "8-16", ">=16")
PAGE0 = ("ms", "loops", "loop_sum", "loop_max", "scans", "scan_sum", "scan_max",
         "i2c_n", "i2c_sum", "i2c_max", "i2c_fail", "rpc_n", "rpc_fail")
PAGE1 = ("rpc_sum", "rpc_max", "rpc_new", "frames", "busy") + tuple(f"loop_h{i}" for i in range(6))
PAGE2 = tuple(f"i2c_h{i}" for i in range(6)) + tuple(f"scan_h{i}" for i in range(6))


class Board:
    def __init__(self, serial):
        self.dev = open_device(serial)
        prefix = b"\xdd" + bytes(4) + secrets.token_bytes(20)
        reply = self._xfer(prefix, lambda r: r[:25] == prefix)
        self.client = int.from_bytes(reply[25:29], "little")
        if self.client in (0, 0xFFFFFFFF):
            raise RuntimeError("invalid client ID")

    def _xfer(self, request, match, timeout=0.5):
        self.dev.write(b"\0" + request.ljust(64, b"\0"))
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            r = bytes(self.dev.read(64, 20))
            if r and match(r):
                return r
        raise TimeoutError("no reply from the board")

    def op(self, target, page=0):
        prefix = b"\xdd" + self.client.to_bytes(4, "little")

        def match(r):
            if r[:5] != prefix:
                return False
            if r[5] == 0xFF:
                raise RuntimeError(f"client wrapper rejected the command: {r[6]}")
            return r[5] == PROTOCOL and r[6] in (0xC3, 0x80 | 0x43)

        r = self._xfer(prefix + bytes((PROTOCOL, 0x43, target, page)), match)
        legacy = r[7] == 2 and r[8] == 1 and r[12:14] == bytes((0, 8))
        if r[6] != 0xC3 or r[11] != 0xA5 or legacy:
            raise RuntimeError("this firmware has no timing counters (build with SVAL_PERF_STATS=yes)")
        return r[7], r[10], struct.unpack_from("<13I", r, 12)

    def half(self, target):
        out = {}
        for page, names in enumerate((PAGE0, PAGE1, PAGE2)):
            status, native, words = self.op(target, page)
            if status:
                return None
            out.update(zip(names, words))
        out["native"] = native
        return out

    def reset(self):
        status, _, _ = self.op(2)
        return status == 0

    def close(self):
        self.dev.close()


def avg(s, k):
    return s[k + "_sum"] / s[k + "_n"] / 1000 if s.get(k + "_n") else 0.0


def report(name, s):
    if s is None:
        return f"{name}: no answer (split link down?)"
    sec = max(s["ms"], 1) / 1000
    s["loop_n"], s["scan_n"] = s["loops"], s["scans"]
    lines = [f"{name}: window {sec:.1f} s",
             f"  matrix scans  {s['scans'] / sec:7.0f}/s  avg gap {avg(s, 'scan'):.2f} ms  max {s['scan_max'] / 1000:.2f} ms",
             f"  main loop     {s['loops'] / sec:7.0f}/s  avg {avg(s, 'loop'):.3f} ms  max {s['loop_max'] / 1000:.2f} ms",
             f"  pad I2C read  {s['i2c_n'] / sec:7.1f}/s  avg {avg(s, 'i2c'):.2f} ms  max {s['i2c_max'] / 1000:.2f} ms"
             f"  busy {s['i2c_sum'] / 10 / max(s['ms'], 1):.1f}% of time  fails {s['i2c_fail']}"]
    if s["rpc_n"] or s["frames"]:
        lines.append(f"  split pad RPC {s['rpc_n'] / sec:7.1f}/s  avg {avg(s, 'rpc'):.2f} ms  max {s['rpc_max'] / 1000:.2f} ms"
                     f"  busy {s['rpc_sum'] / 10 / max(s['ms'], 1):.1f}%  fails {s['rpc_fail']}  new {s['rpc_new']}")
        lines.append(f"  touchpad      {s['frames'] / sec:7.1f} frames/s  endpoint busy {s['busy']}")
    for key, label in (("loop_h", "loop gaps"), ("scan_h", "scan gaps"), ("i2c_h", "I2C reads")):
        lines.append(f"  {label:10s} ms " + "  ".join(f"{b}:{s[f'{key}{i}']}" for i, b in enumerate(BUCKETS)))
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--serial", required=True, help="exact USB serial of the test board")
    ap.add_argument("action", choices=("get", "reset", "watch", "run"))
    ap.add_argument("seconds", nargs="?", type=float, default=10)
    args = ap.parse_args()
    if args.serial in REFUSED:
        ap.error(f"refusing {REFUSED[args.serial]}")
    board = Board(args.serial)
    try:
        if args.action == "reset":
            print("reset" if board.reset() else "reset this half only (other half did not answer)")
        elif args.action == "get":
            print("(touchpad frames: since power-up, not since the reset)")
            print(report("USB half", board.half(0)))
            print(report("other half", board.half(1)))
        elif args.action == "run":
            f0 = board.half(0)["frames"]  # frames_sent is never reset
            board.reset()
            time.sleep(args.seconds)
            s = board.half(0)
            s["frames"] -= f0
            print(report("USB half", s))
            print(report("other half", board.half(1)))
        else:
            # Counters restart every second, so each line is that second alone.
            print("USB half, one line per second (other half: max scan gap)")
            print("  sec  frames/s  scans/s  loops/s  loop max  scan max  I2C avg  I2C max"
                  "  I2C busy  other scan max  (ms)")
            worst = {"loop": 0, "scan": 0, "other": 0}
            board.reset()
            prev_frames = board.half(0)["frames"]
            for t in range(1, int(args.seconds) + 1):
                time.sleep(1)
                d, o = board.half(0), board.half(1)
                board.reset()
                sec = max(d["ms"], 1) / 1000
                frames = d["frames"] - prev_frames  # frames_sent is never reset
                prev_frames = d["frames"]
                other = o["scan_max"] / 1000 if o else float("nan")
                worst["loop"] = max(worst["loop"], d["loop_max"])
                worst["scan"] = max(worst["scan"], d["scan_max"])
                worst["other"] = max(worst["other"], o["scan_max"] if o else 0)
                print(f"{t:5d}  {frames / sec:8.1f}  {d['scans'] / sec:7.0f}  {d['loops'] / sec:7.0f}"
                      f"  {d['loop_max'] / 1000:8.2f}  {d['scan_max'] / 1000:8.2f}  {avg(d, 'i2c'):7.2f}"
                      f"  {d['i2c_max'] / 1000:7.2f}  {d['i2c_sum'] / 10 / max(d['ms'], 1):7.1f}%"
                      f"  {other:14.2f}")
            print(f"worst over the watch: loop {worst['loop'] / 1000:.2f} ms, scan gap "
                  f"{worst['scan'] / 1000:.2f} ms, other half scan gap {worst['other'] / 1000:.2f} ms")
    finally:
        board.close()


if __name__ == "__main__":
    sys.exit(main())
