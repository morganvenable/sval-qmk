"""Read the timing counters of a SVAL_PERF_STATS dev build (raw HID 0xFC op 0x43).

usage: python perf.py --serial EXACT_SERIAL {get,reset,watch,run} [SECONDS]

  get          counters since the last reset, both halves (touchpad frames:
               the total since power-up, which a reset does not clear)
  reset        zero the counters on both halves
  run SECONDS  reset, wait, then print the window's numbers once
  watch [N]    reset, then print one line per second for N seconds (default 10)

Rates are per second over the window: matrix frames (scans), main-loop
passes, pad reads, split fetches of the other pad, touchpad frames. Maximums
are the worst single gap or read in the window. Each half also reports the
scan settings in effect and how often its raw matrix saw a key down, and what
its main loop was doing in the slowest scan gap: the matrix row reads, the
split exchange of every scan, the pad read, the other pad's split fetch, USB
sends, these counters' own reads of the other half, and everything else.

On a native touchpad build the pad read is the contact read alone; on a build
without it, the whole driver call (read and end of session). The counters'
sums are 32-bit: windows must be shorter than 70 minutes.
"""
import argparse
import struct
import sys
import time

from sval_raw import Board

PROTOCOL = 0xFC
MAX_WINDOW_MS = 70 * 60 * 1000
BUCKETS = ("<1", "1-2", "2-4", "4-8", "8-16", ">=16")
PAGE0 = ("ms", "loops", "loop_sum", "loop_max", "scans", "scan_sum", "scan_max", "i2c_n", "i2c_sum", "i2c_max", "i2c_fail", "rpc_n", "rpc_fail")
PAGE1 = ("rpc_sum", "rpc_max", "rpc_new", "frames", "busy") + tuple(f"loop_h{i}" for i in range(6)) + ("sched_lead", "sched_xfer")
PAGE2 = tuple(f"i2c_h{i}" for i in range(6)) + tuple(f"scan_h{i}" for i in range(6)) + ("rpc_hint",)
PAGE3 = ("scan_period", "scan_period_now", "prewait", "postwait", "turbo", "idle_stage", "flipfet", "raw_down", "raw_changes", "sched_wait", "pads", "merge_waits", "host_silent")
SECTIONS = ("touch task", "pad driver call", "sample to touchpad", "touchpad task")
PAGE4 = tuple(f"raw_row{i}" for i in range(5)) + tuple(f"sec{i}_{k}" for i in range(len(SECTIONS)) for k in ("sum", "max"))
# The slowest scan gap, by part (perf.h SVAL_PERF_GAP_*, in that order).
GAP_PARTS = ("other", "matrix", "split sync", "pad read", "split fetch", "USB", "counters")
PAGE5 = ("gap",) + tuple(f"gap_{i}" for i in range(len(GAP_PARTS))) + ("gap_passes", "gap_pass_max", "gap_at_ms", "gap_reads", "gap_fetches")
PAGES = (PAGE0, PAGE1, PAGE2, PAGE3, PAGE4, PAGE5)
STAGES = ("active", "light idle", "deep idle")


class Counters:
    def __init__(self, serial):
        self.board = Board(serial)

    def op(self, target, page=0):
        r = self.board.command(PROTOCOL, 0x43, (target, page))
        if r[6] != 0xC3 or r[11] != 0xA5:
            raise RuntimeError("this firmware has no timing counters (build with SVAL_PERF_STATS=yes)")
        return r[7], r[10], struct.unpack_from("<13I", r, 12)

    def half(self, target):
        out = {}
        for page, names in enumerate(PAGES):
            status, native, words = self.op(target, page)
            if status:
                return None
            out.update(zip(names, words))
        out["native"] = bool(out["pads"] >> 8)
        return out

    def reset(self):
        status, _, _ = self.op(2)
        return status == 0

    def close(self):
        self.board.close()


def avg(s, k):
    return s[k + "_sum"] / s[k + "_n"] / 1000 if s.get(k + "_n") else 0.0


def gap_line(s, at=True):
    """The slowest scan gap and where its time went, largest part first."""
    if not s.get("gap"):
        return "slowest gap   none yet"
    parts = sorted(((s[f"gap_{i}"], n) for i, n in enumerate(GAP_PARTS)), reverse=True)
    spent = ", ".join(f"{n} {us / 1000:.2f}" for us, n in parts if us >= 50)
    when = f" at {s['gap_at_ms'] / 1000:.1f} s" if at else ""
    return (f"slowest gap   {s['gap'] / 1000:.2f} ms{when}: {spent} ms;"
            f" {s['gap_passes']} loop passes (longest {s['gap_pass_max'] / 1000:.2f} ms),"
            f" {s['gap_reads']} pad reads, {s['gap_fetches']} split fetches")


def report(name, s, frames_total=False):
    if s is None:
        return f"{name}: no answer (split link down?)"
    if s["ms"] > MAX_WINDOW_MS:
        return f"{name}: window over 70 minutes; the sums have wrapped: reset first"
    sec = max(s["ms"], 1) / 1000
    s["loop_n"], s["scan_n"] = s["loops"], s["scans"]
    period = f"{s['scan_period']} us" if s["scan_period"] else "unpaced"
    stage = STAGES[s["idle_stage"]] if s["idle_stage"] < 3 else s["idle_stage"]
    read = "contact read" if s["native"] else "read + end of session"
    lines = [
        f"{name}: window {sec:.1f} s", f"  scan settings  period {period} (now {s['scan_period_now']} us, {stage}), "
        f"pre/post wait {s['prewait']}/{s['postwait']} us, turbo {s['turbo']}, "
        f"{'flip-FET' if s['flipfet'] else 'standard'} board", f"  raw matrix     {s['raw_down']} frames with a key down, {s['raw_changes']} changes; last frame rows " + " ".join(f"{s[f'raw_row{i}']:02x}" for i in range(5)),
        f"  matrix scans  {s['scans'] / sec:7.0f}/s  avg gap {avg(s, 'scan'):.2f} ms  max {s['scan_max'] / 1000:.2f} ms", f"  main loop     {s['loops'] / sec:7.0f}/s  avg {avg(s, 'loop'):.3f} ms  max {s['loop_max'] / 1000:.2f} ms",
        f"  pad read      {s['i2c_n'] / sec:7.1f}/s  avg {avg(s, 'i2c'):.2f} ms  max {s['i2c_max'] / 1000:.2f} ms"
        f"  busy {s['i2c_sum'] / 10 / max(s['ms'], 1):.1f}% of time  fails {s['i2c_fail']}  ({read})"
    ]
    if s["rpc_n"] or s["frames"]:
        lines.append(f"  split fetch   {s['rpc_n'] / sec:7.1f}/s  avg {avg(s, 'rpc'):.2f} ms  max {s['rpc_max'] / 1000:.2f} ms"
                     f"  busy {s['rpc_sum'] / 10 / max(s['ms'], 1):.1f}%  fails {s['rpc_fail']}  new {s['rpc_new']}"
                     f"  on news {s['rpc_hint']}")
    if s["native"]:
        hands = [h for bit, h in ((1, "left"), (2, "right")) if s["pads"] & bit]
        frames = f"{s['frames']} frames since power-up" if frames_total else f"{s['frames'] / sec:7.1f} frames/s"
        lines.append(f"  touchpad      {frames}  endpoint busy {s['busy']}"
                     f"  held for the other pad {s['merge_waits']}  host silent {s['host_silent']}"
                     f"  pads: {', '.join(hands) or 'none'}")
        lines.append(f"  pad schedule  lead {s['sched_lead'] / 1000:.2f} ms  bare read {s['sched_xfer'] / 1000:.2f} ms"
                     f"  last stretch {s['sched_wait'] / 1000:.2f} ms")
    if s["native"]:
        lines.append("  time in       " + ", ".join(f"{name} {s[f'sec{i}_sum'] / 10 / max(s['ms'], 1):.1f}% (max {s[f'sec{i}_max'] / 1000:.2f} ms)" for i, name in enumerate(SECTIONS)))
    for key, label in (("loop_h", "loop gaps"), ("scan_h", "scan gaps"), ("i2c_h", "pad reads")):
        lines.append(f"  {label:10s} ms " + "  ".join(f"{b}:{s[f'{key}{i}']}" for i, b in enumerate(BUCKETS)))
    lines.append("  " + gap_line(s))
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--serial", required=True, help="exact USB serial of the board")
    ap.add_argument("action", choices=("get", "reset", "watch", "run"))
    ap.add_argument("seconds", nargs="?", type=float, default=10)
    args = ap.parse_args()
    if args.seconds * 1000 > MAX_WINDOW_MS:
        ap.error("windows must be shorter than 70 minutes")
    c = Counters(args.serial)
    try:
        if args.action == "reset":
            print("reset" if c.reset() else "reset this half only (the other half did not answer)")
        elif args.action == "get":
            print(report("USB half", c.half(0), frames_total=True))
            print(report("other half", c.half(1), frames_total=True))
        elif args.action == "run":
            f0 = c.half(0)["frames"]  # frames_sent is never reset
            c.reset()
            time.sleep(args.seconds)
            s = c.half(0)
            s["frames"] -= f0
            print(report("USB half", s))
            print(report("other half", c.half(1)))
        else:
            # Counters restart every second, so each line is that second alone.
            print("USB half, one line per second (other half: max scan gap)")
            print("  sec  frames/s  scans/s  loops/s  loop max  scan max  read avg  read max"
                  "  read busy  other scan max  (ms)")
            worst = {"loop": 0, "scan": 0, "other": 0}
            worst_gap = {}
            c.reset()
            prev_frames = c.half(0)["frames"]
            for t in range(1, int(args.seconds) + 1):
                time.sleep(1)
                d, o = c.half(0), c.half(1)
                c.reset()
                sec = max(d["ms"], 1) / 1000
                frames = d["frames"] - prev_frames
                prev_frames = d["frames"]
                other = o["scan_max"] / 1000 if o else float("nan")
                worst["loop"] = max(worst["loop"], d["loop_max"])
                worst["scan"] = max(worst["scan"], d["scan_max"])
                worst["other"] = max(worst["other"], o["scan_max"] if o else 0)
                for name, h in (("USB half", d), ("other half", o)):
                    if h and h["gap"] and h["gap"] > worst_gap.get(name, {}).get("gap", 0):
                        worst_gap[name] = dict(h, second=t)
                print(
                    f"{t:5d}  {frames / sec:8.1f}  {d['scans'] / sec:7.0f}  {d['loops'] / sec:7.0f}"
                    f"  {d['loop_max'] / 1000:8.2f}  {d['scan_max'] / 1000:8.2f}  {avg(d, 'i2c'):8.2f}"
                    f"  {d['i2c_max'] / 1000:8.2f}  {d['i2c_sum'] / 10 / max(d['ms'], 1):8.1f}%"
                    f"  {other:14.2f}"
                )
            print(f"worst over the watch: loop {worst['loop'] / 1000:.2f} ms, scan gap "
                  f"{worst['scan'] / 1000:.2f} ms, other half scan gap {worst['other'] / 1000:.2f} ms")
            for name, h in worst_gap.items():
                print(f"  {name}, second {h['second']}: " + gap_line(h, at=False))
    finally:
        c.close()


if __name__ == "__main__":
    sys.exit(main())
