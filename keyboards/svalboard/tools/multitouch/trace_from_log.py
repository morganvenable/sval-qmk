"""Turn a unified_lab.py --log recording into a trace for the firmware recognizer.

  python trace_from_log.py session.jsonl > session.trace
  # in WSL, from the repo root:
  cc -std=c11 tests/sval_touch_gestures/test_touch_gestures.c -o /tmp/tg
  /tmp/tg --replay session.trace 0e

The second argument to --replay is payload byte 0 in hex (0e = swipe and pinch
owned by the firmware, gestures across pads). The replay prints every output
and claim, so swipe and pinch thresholds can be tuned offline against real
contacts. Times are host receipt times in milliseconds.
"""
import json
import sys


def convert(lines, out):
    start = None
    out.write("# ms pad flags mask [id x y] x5 (raw pad counts)\n")
    for line in lines:
        event = json.loads(line)
        if event.get("event") != "frame":
            continue
        ms = event["received"] * 1000
        start = ms if start is None else start
        fields = [str(round(ms - start)), str(event["pad"]), str(event["flags"] & 0x3F), str(event["mask"])]
        for contact in event["contacts"]:
            fields += [str(contact[0]), str(contact[1]), str(contact[2])]
        out.write(" ".join(fields) + "\n")


if __name__ == "__main__":
    with open(sys.argv[1], encoding="utf-8") as f:
        convert(f, sys.stdout)
