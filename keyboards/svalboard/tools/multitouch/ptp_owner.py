"""Get or set who owns the touchpads on a native-touchpad build (SVAL_NATIVE_PTP).

usage: python ptp_owner.py --serial EXACT_SERIAL {get,mouse,host,native}

Owners: mouse (the chip's relative mouse), host (host injection through the
0xFC stream; unified_lab.py and friends need this), native (the Windows
Precision Touchpad interface). The setting is RAM only: the build default
(native) returns at power-up. Also prints the Input Mode, latency and switch
values the host last wrote, and the touchpad's frame counters.
"""
import argparse
import struct
import sys

from capture import open_device
from transport import PROTOCOL, HidDispatcher, StreamClient

OWNERS = {"mouse": 0, "host": 1, "detector": 2, "native": 3}
NAMES = {v: k for k, v in OWNERS.items()}
DAILY_DRIVER = "sval:E465B0621736062B"  # never touch Morgan's daily driver


def not_built(reply):
    """Older firmware answers op 0x42 as an unknown stream op: status 2 and the
    pad geometry (2048, little endian) at bytes 12-13, where a native build
    has the latency and switch bytes (never 8)."""
    return reply[7] == 2 and reply[12] == 0x00 and reply[13] == 0x08


def owner_command(dispatcher, client, action, wanted=0):
    prefix = b"\xdd" + client.client.to_bytes(4, "little")

    def accept(reply):
        if reply[:5] != prefix:
            return None
        if reply[5] == 0xFF:
            raise RuntimeError(f"Client wrapper rejected the command: {reply[6]}")
        if reply[5:7] != bytes((PROTOCOL, 0xC2)):
            return None
        if not_built(reply):
            raise RuntimeError("Firmware has no owner command (built without SVAL_NATIVE_PTP)")
        return reply

    return dispatcher.exchange(prefix + bytes((PROTOCOL, 0x42, action, wanted)), accept)


def describe(reply):
    status, owner, built = reply[7], reply[8], reply[9]
    s = reply[10:26]
    native, mode, latency, switches, bits, contacts = s[0], s[1], s[2], s[3], s[4], s[5]
    frames, busy = struct.unpack_from("<II", s, 6)
    scan = struct.unpack_from("<H", s, 14)[0]
    flags = [name for bit, name in ((1, "reporting"), (2, "mouse-hold"), (4, "left-armed"),
                                    (8, "right-armed"), (16, "release-pending"), (32, "mouse-suppressed"),
                                    (64, "mode3")) if bits & bit]
    return "\n".join((
        f"owner: {NAMES.get(owner, owner)}" + (f" (refused, status {status})" if status else ""),
        f"native touchpad built: {'yes' if built & 1 else 'no'}; native owner active: {'yes' if native else 'no'}",
        f"input mode: {mode} ({'touchpad' if mode == 3 else 'mouse'}), latency: {'high' if latency else 'normal'}, "
        f"surface switch: {switches & 1}, button switch: {(switches >> 1) & 1}",
        f"state: {', '.join(flags) or 'idle'}; contacts reported: {contacts}",
        f"frames sent: {frames}, endpoint busy: {busy}, scan time: {scan}",
    ))


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--serial", required=True, help="exact USB serial of the test board")
    parser.add_argument("action", choices=("get", "mouse", "host", "native"))
    args = parser.parse_args()
    if args.serial == DAILY_DRIVER:
        parser.error("refusing the daily-driver board")
    device = open_device(args.serial)
    dispatcher = HidDispatcher(device, report_size=64)
    try:
        client = StreamClient(dispatcher)
        client.bootstrap()
        if args.action == "get":
            reply = owner_command(dispatcher, client, 0)
        else:
            reply = owner_command(dispatcher, client, 1, OWNERS[args.action])
        print(describe(reply))
        return 1 if reply[7] else 0
    finally:
        dispatcher.close()
        device.close()


if __name__ == "__main__":
    sys.exit(main())
