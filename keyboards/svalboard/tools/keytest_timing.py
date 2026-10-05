#!/usr/bin/env python3
"""On-board checks for the runtime tap-hold, combo and one-shot settings.

Each setting is exercised on and off through the real action path. Original
bindings and settings are journaled first and restored (verified after a
reboot) whether or not the run passes. Test board only: select it by serial.
"""
import argparse
import json
from pathlib import Path
import struct
import sys

from keytest import Device, CLEAR, SELECT_LAYER
from keytest_features import sval, setting_get, setting_set

# Matrix rows: 0 and 5 thumbs ('*'), 1-4 left hand, 6-9 right hand.
MT, SAME, OTHER = (1, 0), (2, 0), (6, 0)
KC_A, KC_B, KC_C = 0x04, 0x05, 0x06
LCTL_T_A = 0x2104
OSM_LSFT = 0x52A2
QSIDS = (2, 7, 22, 23, 24, 25, 26, 27)
TAPPING_TERM, PERMISSIVE, HOLD_OTHER, RETRO, QUICK_TAP, CHORDAL, FLOW_TAP = 7, 22, 23, 24, 25, 26, 27
POSITIONS = [(0, *MT), (0, *SAME), (0, *OTHER)]


def one_shot_get(d):
    r = sval(d, 0x09)
    return [r[2] | r[3] << 8, r[4]]


def one_shot_set(d, timeout, toggle):
    sval(d, 0x0A, struct.pack("<HB", timeout, toggle))


def snapshot(d):
    return dict(serial=d.serial,
                keys=[[*p, d.keymap(*p)] for p in POSITIONS],
                settings={str(q): list(setting_get(d, q)) for q in QSIDS},
                one_shot=one_shot_get(d))


def restore(d, saved):
    assert saved["serial"] == d.serial, "journal belongs to another board"
    if not d.info()["active"]:
        d.begin()
    for layer, row, col, value in saved["keys"]:
        d.keymap(layer, row, col, value)
    for q, v in saved["settings"].items():
        setting_set(d, int(q), bytes(v))
    one_shot_set(d, *saved["one_shot"])
    d.reboot()
    if snapshot(d) != saved:
        raise AssertionError("restore did not survive reboot; keep the journal")


def ev(pos, pressed, delay=0):
    return dict(row=pos[0], col=pos[1], pressed=pressed, delay_ms=delay)


def tap(pos, delay=0, hold=30):
    return [ev(pos, True, delay), ev(pos, False, hold)]


def kb(records):
    return [(r["mods"], r["keys"]) for r in records if r["kind"] in (1, 2)]


def has(seq, mods, key):
    return any(m == mods and key in k for m, k in seq)


def flag(d, qsid, on):
    setting_set(d, qsid, bytes([1 if on else 0, 0]))


def u16(d, qsid, value):
    setting_set(d, qsid, struct.pack("<H", value))


def baseline(d):
    u16(d, TAPPING_TERM, 200)
    for q in (PERMISSIVE, HOLD_OTHER, RETRO, CHORDAL):
        flag(d, q, False)
    u16(d, QUICK_TAP, 200)  # board default: equal to the tapping term
    u16(d, FLOW_TAP, 0)
    one_shot_set(d, 0, 0)


def run(d, out):
    results = []

    def case(name, expect, events, check, settle=700):
        d.command(CLEAR)
        d.events(events)
        seq = kb(d.collect(settle))
        ok = bool(check(seq))
        results.append(dict(name=name, expected=expect, passed=ok, reports=seq))
        Path(out).write_text(json.dumps(results, indent=2) + "\n")
        print(f"{'PASS' if ok else 'FAIL'}  {name}: {seq}", flush=True)

    d.command(SELECT_LAYER, b"\0")
    d.keymap(0, *MT, LCTL_T_A)
    d.keymap(0, *SAME, KC_B)
    d.keymap(0, *OTHER, KC_C)

    # Nested press/release inside the tapping term (opposite hand).
    nested = [ev(MT, True), ev(OTHER, True, 20), ev(OTHER, False, 30), ev(MT, False, 30)]
    baseline(d)
    case("permissive_hold_off", "a then c (tap)", nested, lambda s: has(s, 0, KC_A) and has(s, 0, KC_C) and not any(m for m, _ in s))
    flag(d, PERMISSIVE, True)
    case("permissive_hold_on", "Ctrl+C", nested, lambda s: has(s, 1, KC_C) and not has(s, 0, KC_A))

    # Other key pressed, mod-tap released first (opposite hand).
    rolled = [ev(MT, True), ev(OTHER, True, 20), ev(MT, False, 30), ev(OTHER, False, 10)]
    baseline(d)
    case("hold_on_other_key_off", "a then c", rolled, lambda s: has(s, 0, KC_A) and not any(m for m, _ in s))
    flag(d, HOLD_OTHER, True)
    case("hold_on_other_key_on", "Ctrl+C", rolled, lambda s: has(s, 1, KC_C) and not has(s, 0, KC_A))

    # Chordal Hold: same-hand nested press settles as tap even with permissive hold.
    same = [ev(MT, True), ev(SAME, True, 20), ev(SAME, False, 30), ev(MT, False, 30)]
    baseline(d)
    flag(d, PERMISSIVE, True)
    case("chordal_off_same_hand", "Ctrl+B", same, lambda s: has(s, 1, KC_B))
    flag(d, CHORDAL, True)
    case("chordal_on_same_hand", "a then b (tap)", same, lambda s: has(s, 0, KC_A) and has(s, 0, KC_B) and not any(m for m, _ in s))
    case("chordal_on_opposite_hand", "Ctrl+C", nested, lambda s: has(s, 1, KC_C))

    # Flow Tap: mod-tap pressed 50 ms after a letter settles as tap even when held.
    flow = tap(SAME) + [ev(MT, True, 50), ev(MT, False, 400)]
    baseline(d)
    case("flow_tap_off", "b, then Ctrl held (no a)", flow, lambda s: has(s, 0, KC_B) and any(m == 1 for m, _ in s) and not has(s, 0, KC_A))
    u16(d, FLOW_TAP, 150)
    case("flow_tap_150", "b then a, no Ctrl", flow, lambda s: has(s, 0, KC_B) and has(s, 0, KC_A) and not any(m for m, _ in s))

    # Retro tapping: hold past the term with no other key, release sends the tap.
    held = [ev(MT, True), ev(MT, False, 400)]
    baseline(d)
    case("retro_tapping_off", "Ctrl only, no a", held, lambda s: any(m == 1 for m, _ in s) and not has(s, 0, KC_A))
    flag(d, RETRO, True)
    case("retro_tapping_on", "Ctrl, then a", held, lambda s: any(m == 1 for m, _ in s) and has(s, 0, KC_A))

    # Quick tap: tap then press-and-hold within the term repeats the tap key.
    again = tap(MT) + [ev(MT, True, 60), ev(MT, False, 400)]
    baseline(d)
    u16(d, QUICK_TAP, 0)
    case("quick_tap_0", "a, then Ctrl held", again, lambda s: has(s, 0, KC_A) and any(m == 1 for m, _ in s))
    u16(d, QUICK_TAP, 200)
    case("quick_tap_200", "a, then a held, no Ctrl", again, lambda s: has(s, 0, KC_A) and not any(m for m, _ in s))

    # One-shot Shift timeout.
    d.keymap(0, *MT, OSM_LSFT)
    late = tap(MT) + tap(SAME, 500)
    baseline(d)
    case("oneshot_timeout_0", "Shift+B after 500 ms (no timeout)", late, lambda s: has(s, 2, KC_B))
    one_shot_set(d, 300, 0)
    case("oneshot_timeout_300", "plain b after 500 ms", late, lambda s: has(s, 0, KC_B) and not has(s, 2, KC_B))

    # One-shot tap toggle: two taps lock Shift for the next keys.
    double = tap(MT) + tap(MT, 40) + tap(SAME, 40) + tap(OTHER, 40) + tap(MT, 40)
    baseline(d)
    case("oneshot_toggle_0", "Shift on b only", tap(MT) + tap(SAME, 40) + tap(OTHER, 40), lambda s: has(s, 2, KC_B) and has(s, 0, KC_C))
    one_shot_set(d, 0, 2)
    case("oneshot_toggle_2", "Shift on b and c", double, lambda s: has(s, 2, KC_B) and has(s, 2, KC_C))
    slow = tap(MT) + tap(MT, 40) + tap(SAME, 40) + tap(MT, 300) + tap(OTHER, 40)
    case("oneshot_toggle_unlock_after_300ms", "Shift on b, released by the next tap, c plain", slow,
         lambda s: has(s, 2, KC_B) and has(s, 0, KC_C) and s[-1] == (0, []))
    quick = tap(MT) + tap(MT, 40) + tap(SAME, 40) + tap(MT, 40) + tap(OTHER, 40)
    case("oneshot_toggle_unlock_after_40ms", "Shift on b, released by the next tap, c plain", quick,
         lambda s: has(s, 2, KC_B) and has(s, 0, KC_C) and s[-1] == (0, []))
    return results


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--serial", required=True)
    ap.add_argument("--backup", type=Path, required=True)
    ap.add_argument("--output", type=Path, required=True)
    a = ap.parse_args()
    d = Device(a.serial)
    try:
        d.info()
        saved = snapshot(d)
        with a.backup.open("x") as f:
            json.dump(saved, f, indent=2)
        d.begin()
        results = []
        try:
            results = run(d, a.output)
        finally:
            restore(d, saved)
            print("original bindings and settings restored and verified after reboot", flush=True)
        failed = sum(not r["passed"] for r in results)
        print(f"{len(results) - failed} passed, {failed} failed")
        sys.exit(1 if failed else 0)
    finally:
        d.close()


if __name__ == "__main__":
    main()
