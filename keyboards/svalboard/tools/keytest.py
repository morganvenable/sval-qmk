#!/usr/bin/env python3
"""Closed-loop Svalboard action tests. Requires an SVAL_KEYTEST=yes image and hidapi.

All packets are serialized bare VIA requests. Close other configuration clients.
Capture begins explicitly and stays isolated until an acknowledged normal reboot.
"""
import argparse
import json
from pathlib import Path
import struct
import secrets
import time

CHANNEL = 0x54
INFO, BEGIN, ENQUEUE, RUN, READ, CLEAR, ABORT, REBOOT, STATE, SELECT_LAYER, ACK = range(11)
ERRORS = {1: "invalid request/transition", 2: "capture inactive", 3: "busy/physical key held",
          4: "event queue full", 5: "capture record missing", 6: "session aborted"}


def devices():
    import hid
    return [d for d in hid.enumerate() if d.get("usage_page") == 0xFF61 and d.get("usage") == 0x62]


class Device:
    def __init__(self, serial=None):
        self.serial = serial
        self.handle = None
        self._instrumented = False
        self.open()

    def open(self):
        import hid
        matches = [d for d in devices() if self.serial is None or d.get("serial_number") == self.serial]
        if len(matches) != 1:
            raise RuntimeError(f"Expected one Svalboard, found {len(matches)}; use --serial (see list)")
        self.serial = matches[0].get("serial_number")
        self.handle = hid.device()
        self.handle.open_path(matches[0]["path"])

    def close(self):
        self._instrumented = False
        if self.handle:
            self.handle.close()
            self.handle = None

    def _exchange(self, request):
        if not 1 <= len(request) <= 32:
            raise ValueError("Raw HID request must contain 1..32 bytes")
        packet = bytes(request).ljust(32, b"\0")
        if self.handle.write(b"\0" + packet) != 33:
            raise RuntimeError("Incomplete HID write")
        reply = bytes(self.handle.read(32, 2000))
        if len(reply) != 32:
            raise RuntimeError("Missing/short HID reply")
        if reply[0] != packet[0]:
            raise RuntimeError(f"Unexpected/unsupported reply: {reply.hex()}")
        return reply

    def exchange(self, request):
        if request and request[0] == 0xDF:
            if len(request) > 27:
                raise ValueError("Wrapped Sval payload must fit 27 bytes")
            nonce = secrets.token_bytes(20)
            boot = self._exchange(b"\xDD" + bytes(4) + nonce)
            if boot[1:5] != bytes(4) or boot[5:25] != nonce or boot[25:29] in (bytes(4), b"\xFF" * 4):
                raise RuntimeError("Invalid client bootstrap response")
            response = self._exchange(b"\xDD" + boot[25:29] + bytes(request))
            if response[1:5] != boot[25:29] or response[5] != 0xDF:
                raise RuntimeError("Wrapped Sval command failed")
            return response[5:]
        return self._exchange(request)

    def command(self, op, args=b""):
        if len(args) > 22:
            raise ValueError("Diagnostic argument too long")
        if op != INFO and not getattr(self, "_instrumented", False):
            self.info()  # Read-only probe before any test opcode reaches older firmware.
        command = 8 if op == INFO else 7
        reply = self.exchange(bytes([command, CHANNEL, op]) + bytes(args))
        if reply[1:3] != bytes([CHANNEL, op]):
            raise RuntimeError("Unexpected diagnostic reply")
        if reply[3]:
            raise RuntimeError(ERRORS.get(reply[3], f"status {reply[3]}"))
        return reply[4:26]

    def info(self):
        r = self.command(INFO)
        if r[0] != 1:
            raise RuntimeError("Unsupported keytest protocol; build with SVAL_KEYTEST=yes")
        self._instrumented = True
        return dict(version=r[0], active=bool(r[1] & 1), running=bool(r[1] & 2),
                    aborted=bool(r[1] & 4), rows=r[2], cols=r[3], capacity=r[4],
                    queued=r[6], layers=r[19], first=struct.unpack_from("<I", r, 7)[0],
                    next=struct.unpack_from("<I", r, 11)[0], lost=struct.unpack_from("<I", r, 15)[0])

    def begin(self):
        self.info()
        self.command(BEGIN, b"TEST")

    def reboot(self):
        if not self.serial:
            raise RuntimeError("Reconnecting requires a unique device serial")
        self.command(REBOOT)
        self.close()
        time.sleep(1)
        deadline = time.monotonic() + 15
        while True:
            try:
                self.open()
                if self.info()["active"]:
                    raise RuntimeError("Waiting for fresh firmware session")
                return
            except (OSError, RuntimeError):
                self.close()
                if time.monotonic() >= deadline:
                    raise RuntimeError("Board did not reconnect after reboot")
                time.sleep(0.25)

    def keymap(self, layer, row, col, value=None):
        request = [4 if value is None else 5, layer, row, col]
        if value is not None:
            request += [value >> 8, value & 255]
        r = self.exchange(request)
        if r[1:4] != bytes([layer, row, col]):
            raise RuntimeError("Unexpected keymap reply")
        return (r[4] << 8) | r[5]

    def state(self):
        r = self.command(STATE)
        layers, default = struct.unpack_from("<II", r)
        return dict(layers=layers, default_layers=default, mods=r[8], weak_mods=r[9])

    def events(self, events):
        # Batch before RUN: transport latency does not determine hold intervals.
        for e in events:
            self.command(ENQUEUE, struct.pack("<HBBB", e.get("delay_ms", 0), e["row"], e["col"], int(e["pressed"])))
        self.command(RUN)

    def records(self, cursor=0):
        info = self.info()
        if info["lost"] or cursor < info["first"]:
            raise RuntimeError("Report capture overflow; evidence is incomplete")
        result = []
        for seq in range(cursor, info["next"]):
            data = bytearray()
            first = None
            while first is None or len(data) < first[1]:
                r = self.command(READ, struct.pack("<IB", seq, len(data)))
                if first is None:
                    first = r
                if r[:10] != first[:10] or r[10] != len(data):
                    raise RuntimeError("Capture changed while reading")
                data.extend(r[11:11 + min(11, r[1] - len(data))])
            result.append(decode_record(seq, first[0], struct.unpack_from("<I", first, 2)[0], data))
        self.command(ACK, struct.pack("<I", info["next"]))
        return result, info["next"]

    def collect(self, settle_ms=300, timeout=20):
        records, cursor = [], 0
        deadline = time.monotonic() + timeout
        idle_since = None
        while time.monotonic() < deadline:
            batch, cursor = self.records(cursor)
            records.extend(batch)
            info = self.info()
            if info["aborted"]:
                raise RuntimeError("Firmware aborted the test")
            if not info["running"] and not info["queued"]:
                if idle_since is None:
                    idle_since = time.monotonic()
                if time.monotonic() - idle_since >= settle_ms / 1000:
                    # One final drain includes reports emitted during the last wait.
                    batch, cursor = self.records(cursor)
                    return records + batch
            else:
                idle_since = None
            time.sleep(0.005)
        raise RuntimeError("Event sequence did not complete")


def decode_record(seq, kind, timestamp, data):
    r = dict(seq=seq, kind=kind, time_ms=timestamp, bytes=list(data))
    if kind in (1, 2):
        r["mods"] = data[0]
        r["keys"] = sorted(set(k for k in data[1:] if k)) if kind == 1 else [
            k for k in range(8 * (len(data) - 1)) if data[1 + k // 8] & (1 << (k % 8))]
    elif kind == 3:
        r["buttons"] = data[0]
        r.update(zip(("x", "y", "h", "v"), struct.unpack_from("<hhhh", data, 1)))
    elif kind == 4:
        r.update(report_id=data[0], usage=data[1] | data[2] << 8)
    elif kind == 5:
        r.update(row=data[0], col=data[1], pressed=bool(data[2]))
    return r


def expect_tap(records, usage, mods=0):
    keyboard = [r for r in records if r["kind"] in (1, 2)]
    presses = [i for i, r in enumerate(keyboard) if r["keys"] == [usage] and r["mods"] == mods]
    if not presses or not any(not r["keys"] and not r["mods"] for r in keyboard[presses[0] + 1:]):
        raise AssertionError(f"Expected usage {usage:#x}/mods {mods:#x}, then release; got {keyboard}")
    if any(r["keys"] and (r["keys"] != [usage] or r["mods"] != mods) for r in keyboard):
        raise AssertionError(f"Unexpected keyboard output: {keyboard}")


def exercise(device, row, col, usage, mods=0, hold_ms=30, settle_ms=300):
    device.command(CLEAR)
    device.events([dict(row=row, col=col, pressed=True),
                   dict(row=row, col=col, pressed=False, delay_ms=hold_ms)])
    records = device.collect(settle_ms)
    expect_tap(records, usage, mods)
    return records


def verify_binding(device, args):
    """Snapshot → write/read → behavior → reboot/read/behavior → restore/reboot."""
    if not 4 <= args.keycode <= 0xDF:
        raise ValueError("verify-binding accepts basic HID keyboard usages 0x04..0xDF; use run for complex bindings")
    info = device.info()
    if not (0 <= args.row < info["rows"] and 0 <= args.col < info["cols"]):
        raise ValueError("Matrix position out of range")
    if not 0 <= args.layer < info["layers"]:
        raise ValueError("Layer out of range for this build")
    if not device.serial:
        raise ValueError("A unique serial is required for persistence tests")
    original = device.keymap(args.layer, args.row, args.col)
    backup = dict(serial=device.serial, layer=args.layer, row=args.row, col=args.col, keycode=original)
    # Exclusive creation preserves an earlier interrupted test's recovery record.
    with Path(args.backup).open("x") as f:
        json.dump(backup, f, indent=2)
    result = dict(backup=backup)
    changed = False
    started = False
    try:
        device.begin()
        started = True
        device.command(SELECT_LAYER, bytes([args.layer]))
        changed = True  # A lost write reply still requires rollback.
        device.keymap(args.layer, args.row, args.col, args.keycode)
        if device.keymap(args.layer, args.row, args.col) != args.keycode:
            raise AssertionError("Immediate keymap readback mismatch")
        result["before_reboot"] = exercise(device, args.row, args.col, args.keycode)
        device.reboot()
        device.begin()
        device.command(SELECT_LAYER, bytes([args.layer]))
        if device.keymap(args.layer, args.row, args.col) != args.keycode:
            raise AssertionError("Binding did not survive reboot")
        result["after_reboot"] = exercise(device, args.row, args.col, args.keycode)
    finally:
        if changed:
            # Leave the backup intact on any failure, including failed rollback.
            if not device.handle:
                device.open()
            if not device.info()["active"]:
                device.begin()
            device.keymap(args.layer, args.row, args.col, original)
            if device.keymap(args.layer, args.row, args.col) != original:
                raise AssertionError("Rollback readback failed; use the backup with restore")
            device.reboot()
            if device.keymap(args.layer, args.row, args.col) != original:
                raise AssertionError("Rollback did not survive reboot")
        elif started:
            device.reboot()
    result["restored"] = True
    return result


def run_scenario(device, steps):
    """Extensible behavioral assertions; raw writes in scenarios are intentional."""
    results, records = [], []
    device.begin()
    try:
        for step in steps:
            op = step["op"]
            if op == "exchange":
                response = list(device.exchange(step["request"]))
                if "expect" in step and response[:len(step["expect"])] != step["expect"]:
                    raise AssertionError(f"Packet mismatch: {response}")
                results.append(dict(response=response))
            elif op == "layer":
                device.command(SELECT_LAYER, bytes([step["layer"]]))
            elif op == "events":
                device.command(CLEAR)
                device.events(step["events"])
                records = device.collect(step.get("settle_ms", 300), step.get("timeout", 20))
                results.append(dict(records=records))
            elif op == "expect_tap":
                expect_tap(records, step["usage"], step.get("mods", 0))
            elif op == "expect_report":
                if not any(all(r.get(k) == v for k, v in step["match"].items()) for r in records):
                    raise AssertionError(f"Expected report {step['match']}; got {records}")
            elif op == "state":
                state = device.state()
                if any(state.get(k) != v for k, v in step.get("expect", {}).items()):
                    raise AssertionError(f"Unexpected runtime state: {state}")
                results.append(state)
            elif op == "reboot":
                device.reboot()
                device.begin()
            else:
                raise ValueError(f"Unknown scenario operation: {op}")
    finally:
        if device.handle and device.info()["active"]:
            device.reboot()
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", help="exact device serial from list")
    parser.add_argument("--output", help="write machine-readable evidence JSON")
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("list")
    sub.add_parser("info")
    sub.add_parser("bootloader", help="acknowledge then enter UF2 bootloader for the next build")
    sub.add_parser("reboot", help="end an existing captured/aborted session")
    verify = sub.add_parser("verify-binding")
    verify.add_argument("--row", type=int, required=True)
    verify.add_argument("--col", type=int, required=True)
    verify.add_argument("--layer", type=int, default=0)
    verify.add_argument("--keycode", type=lambda s: int(s, 0), default=4)
    verify.add_argument("--backup", required=True, help="new file for original binding; retained for recovery")
    run = sub.add_parser("run")
    run.add_argument("scenario", type=Path)
    restore = sub.add_parser("restore")
    restore.add_argument("backup", type=Path)
    args = parser.parse_args()
    if args.command == "list":
        print(json.dumps([{k: d.get(k) for k in ("serial_number", "product_string", "vendor_id", "product_id")} for d in devices()], indent=2))
        return
    device = Device(args.serial)
    try:
        if args.command == "info":
            result = device.info()
        elif args.command == "reboot":
            device.reboot()
            result = device.info()
        elif args.command == "bootloader":
            if not device.info()["active"]:
                device.begin()
            device.command(REBOOT, b"\x01")
            result = dict(bootloader_requested=True)
        elif args.command == "verify-binding":
            result = verify_binding(device, args)
        elif args.command == "run":
            result = run_scenario(device, json.loads(args.scenario.read_text()))
        elif args.command == "restore":
            saved = json.loads(args.backup.read_text())
            if saved["serial"] != device.serial:
                raise ValueError("Backup belongs to a different board")
            if not device.info()["active"]:
                device.begin()
            device.keymap(saved["layer"], saved["row"], saved["col"], saved["keycode"])
            device.reboot()
            actual = device.keymap(saved["layer"], saved["row"], saved["col"])
            if actual != saved["keycode"]:
                raise AssertionError("Restored binding did not survive reboot")
            result = dict(restored=True)
        output = json.dumps(result, indent=2)
        if args.output:
            Path(args.output).write_text(output + "\n")
        print(output)
    except Exception as exc:
        if args.output:
            Path(args.output).write_text(json.dumps(dict(passed=False, error=str(exc)), indent=2) + "\n")
        raise
    finally:
        device.close()


if __name__ == "__main__":
    main()
