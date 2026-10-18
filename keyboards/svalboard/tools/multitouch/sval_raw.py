"""Raw HID access to one Svalboard for the touchpad dev tools (dev builds only).

Opens the Sval raw HID interface of the board with an exact USB serial and
talks through the Sval client wrapper (0xDD): one bootstrap, then requests
[0xDD][client id u32][protocol][op][args...] with replies matched on the
same prefix. Needs hidapi (pip install hidapi).
"""
import secrets
import sys
import time

VID, PID = 0x303A, 0x4044
REPORT = 64


def open_device(serial):
    import hid
    matches = [d for d in hid.enumerate(VID, PID) if d.get("serial_number") == serial and d.get("usage_page") == 0xFF61 and d.get("usage") == 0x62]
    if len(matches) != 1:
        raise RuntimeError(f"expected one raw HID interface for {serial}; found {len(matches)}")
    device = hid.device()
    device.open_path(matches[0]["path"])
    return device


class Board:
    def __init__(self, serial, device=None):
        self.dev = device or open_device(serial)
        prefix = b"\xdd" + bytes(4) + secrets.token_bytes(20)
        reply = self.exchange(prefix, lambda r: r[:25] == prefix)
        self.client = int.from_bytes(reply[25:29], "little")
        if self.client in (0, 0xFFFFFFFF):
            raise RuntimeError("invalid client ID")

    def exchange(self, request, match, timeout=0.5):
        self.dev.write(b"\0" + request.ljust(REPORT, b"\0"))
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            r = bytes(self.dev.read(REPORT, 20))
            if r and match(r):
                return r
        raise TimeoutError("no reply from the board")

    def command(self, protocol, op, args=b""):
        """One request; the reply matched on the client prefix and protocol."""
        prefix = b"\xdd" + self.client.to_bytes(4, "little")

        def match(r):
            if r[:5] != prefix:
                return False
            if r[5] == 0xFF:
                raise RuntimeError(f"the client wrapper rejected the command: {r[6]}")
            return r[5] == protocol

        return self.exchange(prefix + bytes((protocol, op)) + bytes(args), match)

    def close(self):
        self.dev.close()


def fail(msg):
    print(msg, file=sys.stderr)
    sys.exit(2)
