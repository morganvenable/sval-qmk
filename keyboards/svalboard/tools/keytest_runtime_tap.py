#!/usr/bin/env python3
"""Verify runtime toggle/delay/Grave Escape settings on a serial-selected test board.

Requires SVAL_KEYTEST firmware. Every mutation is journaled first, and original
settings, bindings and macro bytes are restored and checked after reboot.
"""
import argparse
import json
from pathlib import Path
import struct
import sys

# Also support the isolated Python runtime shipped with Keybard Host.
sys.path.insert(0, str(Path(__file__).resolve().parent))

from keytest import Device, CLEAR, SELECT_LAYER
from keytest_features import sval, setting_get, setting_set

KEY, MOD, SHIFT = (1, 0), (2, 0), (3, 0)
POSITIONS = [(0, *KEY), (1, *KEY), (0, *MOD), (0, *SHIFT)]
QSIDS = (1, 7, 18, 19, 20, 22, 23, 24, 25, 26, 27)


def macro(d, value=None):
    if value is None:
        r = sval(d, 0x1F, struct.pack('<IB', 0, 16))
        assert r[6] == 16
        return list(r[7:23])
    assert len(value) <= 16
    assert sval(d, 0x20, struct.pack('<IB', 0, len(value)) + bytes(value))[2] == 0


def snapshot(d):
    return dict(serial=d.serial, keys=[[*p, d.keymap(*p)] for p in POSITIONS], settings={str(q): list(setting_get(d, q)) for q in QSIDS}, macro=macro(d))


def restore(d, saved):
    if saved['serial'] != d.serial:
        raise ValueError('Recovery journal belongs to another board')
    if not d.info()['active']:
        d.begin()
    for layer, row, col, code in saved['keys']:
        d.keymap(layer, row, col, code)
    for q, value in saved['settings'].items():
        setting_set(d, int(q), bytes(value))
    macro(d, saved['macro'])
    d.reboot()
    if snapshot(d) != saved:
        raise AssertionError('Restoration did not survive reboot; keep the recovery journal')


def event(pos, pressed, delay=0):
    return dict(row=pos[0], col=pos[1], pressed=pressed, delay_ms=delay)


def tap(pos=KEY, delay=0, hold=30):
    return [event(pos, True, delay), event(pos, False, hold)]


def duration(records, usage):
    reports = [r for r in records if r['kind'] in (1, 2)]
    down = next((i for i, r in enumerate(reports) if usage in r['keys']), None)
    assert down is not None, f'No press of usage {usage}'
    up = next((r for r in reports[down + 1:] if usage not in r['keys']), None)
    assert up is not None, f'No release of usage {usage}'
    assert not reports[-1]['keys'] and not reports[-1]['mods'], 'Stuck output after sequence'
    return (up['time_ms'] - reports[down]['time_ms']) & 0xFFFFFFFF


def run(d, output):
    results = dict(cases=[], restored=False)

    def save():
        output.write_text(json.dumps(results, indent=2) + '\n')

    def check(name, events, assertion, settle=50):
        d.command(CLEAR)
        d.events(events)
        records = d.collect(settle)
        row = dict(name=name, records=records)
        try:
            detail = assertion(records)
            row.update(passed=True, measured=detail)
        except AssertionError as exc:
            row.update(passed=False, error=str(exc))
        results['cases'].append(row)
        save()
        print(('PASS ' if row['passed'] else 'FAIL ') + name, flush=True)

    def timed(expected, usage):
        def assertion(records):
            elapsed = duration(records, usage)
            assert expected <= elapsed <= expected + 10, f'Expected {expected}..{expected + 10} ms, got {elapsed}'
            return dict(duration_ms=elapsed)

        return assertion

    def setq(q, value):
        raw = struct.pack('<H', value)
        setting_set(d, q, raw)
        assert setting_get(d, q) == raw, f'Setting {q} readback mismatch'

    def phases(q, value):
        setq(q, value)
        yield 'immediate'
        d.reboot()
        assert setting_get(d, q) == struct.pack('<H', value), 'Setting lost at reboot'
        d.begin()
        d.command(SELECT_LAYER, b'\0')
        yield 'reboot'

    # Keep other tap-hold controls from changing which path a probe takes.
    setq(7, 200)
    setq(25, 200)
    for q in (22, 23, 24, 26, 27):
        setq(q, 0)
    d.command(SELECT_LAYER, b'\0')

    d.keymap(0, *KEY, 0x52C1)  # TT(1)
    d.keymap(1, *KEY, 1)  # transparent
    for count in (5, 3, 1, 0):
        for phase in phases(20, count):
            effective = count or 1
            for n in range(1, effective + 1):

                def layer_check(records, n=n):
                    state = d.state()
                    assert state['layers'] == (2 if n == effective else 0), state
                    return state

                check(f'toggle_{count}_{phase}_tap_{n}', tap(), layer_check, settle=0)
            d.command(SELECT_LAYER, b'\0')

            def held(records):
                state = d.state()
                assert state['layers'] == 0, state
                return state

            check(f'toggle_{count}_{phase}_held_release', tap(delay=300, hold=300), held)

    d.keymap(0, *KEY, 0x7700)  # macro 0, invokes real tap_code / send_string
    for delay in (0, 37, 256, 300):
        for phase in phases(18, delay):
            macro(d, b'\x01\x01\x04\0')  # SS_TAP(X_A) -> tap_code
            check(f'tap_code_{delay}_{phase}', tap(delay=300), timed(delay, 4))
            macro(d, b'a\0')  # plain text -> send_string_with_delay
            check(f'text_macro_{delay}_{phase}', tap(delay=300), timed(delay, 4))

    setq(18, 0)
    for delay in (0, 80, 130, 300):
        for phase in phases(19, delay):
            d.keymap(0, *KEY, 0x7700)
            macro(d, b'\x01\x01\x39\0')  # SS_TAP(X_CAPS)
            check(f'caps_macro_{delay}_{phase}', tap(delay=300), timed(delay, 0x39))
            for name, code in [('mod_tap', 0x2139), ('layer_tap', 0x4139)]:
                d.keymap(0, *KEY, code)
                check(f'caps_{name}_{delay}_{phase}', tap(delay=300), timed(delay, 0x39))

    d.keymap(0, *KEY, 0x7C16)  # QK_GRAVE_ESCAPE
    for bit, mods in enumerate([(0xE2, 0xE1), (0xE0, 0xE1), (0xE3,), (0xE1,)]):
        for enabled in (False, True):
            for phase in phases(1, 1 << bit if enabled else 0):
                events = []
                for pos, code in zip((MOD, SHIFT), mods):
                    d.keymap(0, *pos, code)
                    events.append(event(pos, True, 20))
                events += tap(delay=20)
                events += [event(pos, False, 20) for pos in reversed((MOD, SHIFT)[:len(mods)])]
                expected, forbidden = (0x29, 0x35) if enabled else (0x35, 0x29)

                def grave(records, expected=expected, forbidden=forbidden):
                    duration(records, expected)
                    assert not any(forbidden in r.get('keys', []) for r in records)
                    return dict(usage=expected)

                check(f'grave_bit_{bit}_{enabled}_{phase}', events, grave)
    return results


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--serial', required=True)
    ap.add_argument('--backup', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--restore', action='store_true')
    a = ap.parse_args()
    d = Device(a.serial)
    try:
        d.info()
        if a.restore:
            restore(d, json.loads(a.backup.read_text()))
            return
        saved = snapshot(d)
        with a.backup.open('x') as f:
            json.dump(saved, f, indent=2)
        results = dict(cases=[], restored=False)
        try:
            d.begin()
            results = run(d, a.output)
        finally:
            restore(d, saved)
            if a.output.exists():
                results = json.loads(a.output.read_text())
            results['restored'] = True
            a.output.write_text(json.dumps(results, indent=2) + '\n')
            print('Original settings, bindings and macro bytes restored and verified after reboot', flush=True)
        if not results['cases'] or any(not c['passed'] for c in results['cases']):
            raise SystemExit(1)
    finally:
        d.close()


if __name__ == '__main__':
    main()
