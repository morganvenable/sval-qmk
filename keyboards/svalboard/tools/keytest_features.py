#!/usr/bin/env python3
"""On-board feature characterization with a durable restore journal.

Runs only against a serial-selected SVAL_KEYTEST image. A nonzero exit can mean
that firmware accepted a setting but failed its behavioral assertion; see JSON.
"""
import argparse
import json
from pathlib import Path
import struct
import sys

from keytest import Device, CLEAR, SELECT_LAYER, expect_tap

POSITIONS = [(0, 0, 0), (0, 0, 1), (0, 0, 2), (1, 0, 1)]
TABLES = [(1, 2, 255, 10), (3, 4, 255, 12)]


def sval(device, op, args=b""):
    reply = device.exchange(bytes([0xDF, op]) + args)
    if reply[:2] != bytes([0xDF, op]):
        raise RuntimeError(f"Unexpected Sval response: {reply.hex()}")
    return reply


def table_get(device, op, index, size):
    return sval(device, op, struct.pack("<H", index))[4:4 + size]


def table_set(device, op, index, value):
    if sval(device, op, struct.pack("<H", index) + bytes(value))[2]:
        raise RuntimeError("Feature-table write failed")


def setting_get(device, qsid):
    reply = sval(device, 0x11, struct.pack("<H", qsid))
    if reply[2]:
        raise RuntimeError("Setting read failed")
    return reply[3:5]


def setting_set(device, qsid, value):
    if sval(device, 0x12, struct.pack("<H", qsid) + bytes(value))[2]:
        raise RuntimeError("Setting write failed")


def macro_get(device):
    reply = sval(device, 0x1F, struct.pack("<IB", 0, 3))
    if reply[6] != 3:
        raise RuntimeError("Macro read failed")
    return reply[7:10]


def macro_set(device, value):
    if sval(device, 0x20, struct.pack("<IB", 0, len(value)) + bytes(value))[2]:
        raise RuntimeError("Macro write failed")


def snapshot(device):
    return dict(serial=device.serial,
                keys=[[*p, device.keymap(*p)] for p in POSITIONS],
                tables=[dict(get=g, set=s, index=i, value=list(table_get(device, g, i, n))) for g, s, i, n in TABLES],
                settings={str(q): list(setting_get(device, q)) for q in (2, 7)},
                macro_prefix=list(macro_get(device)))


def restore(device, saved):
    if saved["serial"] != device.serial:
        raise ValueError("Recovery journal belongs to another board")
    if not device.info()["active"]:
        device.begin()
    for layer, row, col, value in saved["keys"]:
        device.keymap(layer, row, col, value)
    for entry in saved["tables"]:
        table_set(device, entry["set"], entry["index"], entry["value"])
    for qsid, value in saved["settings"].items():
        setting_set(device, int(qsid), value)
    macro_set(device, saved["macro_prefix"])
    device.reboot()
    if snapshot(device) != saved:
        raise AssertionError("Restoration did not survive reboot; retain the recovery journal")


def event(col, pressed, delay=0):
    return dict(row=0, col=col, pressed=pressed, delay_ms=delay)


def tap(hold=30, col=0):
    return [event(col, True), event(col, False, hold)]


def check_modifier_hold(records):
    reports = [r for r in records if r["kind"] in (1, 2)]
    assert any(r["mods"] == 1 and not r["keys"] for r in reports), "No Left Control hold report"
    assert reports and not reports[-1]["mods"] and not reports[-1]["keys"], "Missing final release"
    assert not any(r["keys"] for r in reports), "Tap key emitted during a hold"


def check_macro(records):
    reports = [r for r in records if r["kind"] in (1, 2)]
    down = [r["keys"] for r in reports if r["keys"]]
    assert down == [[4], [5]], f"Expected a then b, got {down}"
    assert reports and not reports[-1]["keys"] and not reports[-1]["mods"], "Missing macro release"


def characterize(device, output):
    results = dict(cases=[], restored=False)

    def save():
        Path(output).write_text(json.dumps(results, indent=2) + "\n")

    def sequence(name, expected, events, check, settle=700):
        device.command(CLEAR)
        device.events(events)
        records = device.collect(settle)
        case = dict(name=name, expected=expected, records=records)
        try:
            check(records)
            case["passed"] = True
        except AssertionError as exc:
            case.update(passed=False, error=str(exc))
        results["cases"].append(case)
        save()
        print(f"{'PASS' if case['passed'] else 'FAIL'} {name}", flush=True)

    device.command(SELECT_LAYER, b"\0")
    # Momentary layer resolves the other key through the real layer lookup.
    device.keymap(0, 0, 0, 0x5221)
    device.keymap(0, 0, 1, 104)
    device.keymap(1, 0, 1, 105)
    sequence("momentary_layer", "F14 from layer 1, then release",
             [event(0, True), event(1, True, 20), event(1, False, 30), event(0, False, 20)],
             lambda r: expect_tap(r, 105))
    assert device.state()["layers"] == 0, "Momentary layer remained enabled"

    # Existing tap/hold engine controls before testing a changed runtime term.
    device.keymap(0, 0, 0, 0x2104)  # LCTL_T(KC_A)
    setting_set(device, 7, struct.pack("<H", 100))
    sequence("mod_tap_short_control", "A tap at 30 ms", tap(30), lambda r: expect_tap(r, 4))
    sequence("mod_tap_long_control", "Left Control at 350 ms", tap(350), check_modifier_hold)
    setting_set(device, 7, struct.pack("<H", 600))
    assert setting_get(device, 7) == struct.pack("<H", 600), "Tapping-term immediate readback failed"
    device.reboot()
    device.begin()
    device.command(SELECT_LAYER, b"\0")
    assert setting_get(device, 7) == struct.pack("<H", 600), "Tapping term did not persist"
    results["tapping_term_600_saved_after_reboot"] = True
    sequence("runtime_tapping_term_600", "A tap at 300 ms, below the saved 600 ms term", tap(300), lambda r: expect_tap(r, 4))

    # Macro zero uses the existing macro protocol and playback engine.
    macro_set(device, b"ab\0")
    device.keymap(0, 0, 0, 0x7700)
    sequence("macro_playback", "a then b, each released", tap(), check_macro)

    td = struct.pack("<5H", 105, 106, 107, 108, 0x8000 | 500)
    table_set(device, 2, 255, td)
    assert table_get(device, 1, 255, 10) == td, "Tap-dance readback failed"
    device.keymap(0, 0, 0, 0x57FF)
    sequence("tap_dance_single", "F14 single tap", tap(), lambda r: expect_tap(r, 105))
    sequence("tap_dance_double", "F16 double tap",
             [event(0, True), event(0, False, 30), event(0, True, 40), event(0, False, 30)],
             lambda r: expect_tap(r, 107))
    sequence("tap_dance_hold", "F15 long hold", tap(800), lambda r: expect_tap(r, 106))
    sequence("tap_dance_custom_term_500", "F14 tap at 300 ms, below the saved 500 ms term",
             tap(300), lambda r: expect_tap(r, 105))

    device.keymap(0, 0, 0, 104)
    device.keymap(0, 0, 1, 105)
    combo = struct.pack("<6H", 104, 105, 0, 0, 106, 0x8000)
    table_set(device, 4, 255, combo)
    chord = lambda gap: [event(0, True), event(1, True, gap), event(0, False, 30), event(1, False, 10)]
    sequence("combo_close_control", "F15 chord at 5 ms spacing", chord(5), lambda r: expect_tap(r, 106))
    combo = struct.pack("<6H", 104, 105, 0, 0, 106, 0x8000 | 200)
    table_set(device, 4, 255, combo)
    assert table_get(device, 3, 255, 12) == combo, "Combo readback failed"
    sequence("combo_custom_term_200", "F15 chord at 100 ms spacing", chord(100), lambda r: expect_tap(r, 106))
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", required=True)
    parser.add_argument("--backup", type=Path, required=True, help="new recovery journal (or existing journal with --restore)")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--restore", action="store_true")
    args = parser.parse_args()
    if not args.restore and args.output is None:
        parser.error("--output is required for a characterization run")
    device = Device(args.serial)
    try:
        device.info()  # Require instrumentation before any writes.
        if args.restore:
            restore(device, json.loads(args.backup.read_text()))
            print("Restored and verified after reboot")
            return
        saved = snapshot(device)
        with args.backup.open("x") as f:
            json.dump(saved, f, indent=2)
        results = None
        device.begin()
        try:
            results = characterize(device, args.output)
        except Exception as exc:
            results = json.loads(args.output.read_text()) if args.output.exists() else dict(cases=[])
            results["error"] = str(exc)
            raise
        finally:
            restore(device, saved)
            if results is not None:
                results["restored"] = True
                args.output.write_text(json.dumps(results, indent=2) + "\n")
        failures = sum(not c["passed"] for c in results["cases"])
        print(f"{len(results['cases']) - failures} passed, {failures} failed; original state restored", flush=True)
        if failures:
            sys.exit(1)
    finally:
        device.close()


if __name__ == "__main__":
    main()
