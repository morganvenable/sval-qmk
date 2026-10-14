"""Touch gesture settings (Sval 0xDF 0x2E), payload v1. No OS dependencies.

The firmware is the single source of truth for who owns scroll, pinch and swipe.
This module reads and writes its 21-byte payload; see
keyboards/svalboard/docs/protocol.md, "Touch gestures".
"""
from dataclasses import dataclass, fields, replace
import struct

COMMAND = 0x2E
VERSION, SIZE = 1, 21
GET, SET, DEFAULTS, STATUS = 0, 1, 2, 3
STATUS_TEXT = {0: "ok", 1: "bad version or length", 2: "a value is out of range",
               3: "this firmware has no touch support"}
OUTPUTS = ("swipe_left", "swipe_right", "swipe_up", "swipe_down", "pinch_out", "pinch_in")
GESTURES = ("none",) + OUTPUTS

# name: (byte bit, value when set, value when clear)
OWNER_BITS = {"scroll_owner": (0x01, "chip", "windows"),
              "swipe_owner": (0x02, "firmware", "windows"),
              "pinch_owner": (0x04, "firmware", "windows")}
OPTION_BITS = {"span_pads": 0x08, "host_always_merge": 0x10}
# name: (minimum, maximum, payload byte); scaled values are stored as shown.
RANGES = {"swipe_fingers": (3, 5), "sustain_min": (1, 5), "swipe_mm": (4, 40),
          "swipe_ratio_x10": (10, 50), "settle_ms": (0, 200), "swipe_window_10ms": (10, 200),
          "swipe_repeat_mm": (0, 40), "pinch_start_x2": (2, 40), "pinch_step_x2": (1, 40)}


@dataclass(frozen=True)
class GestureConfig:
    scroll_owner: str = "windows"
    swipe_owner: str = "windows"
    pinch_owner: str = "windows"
    span_pads: bool = True
    host_always_merge: bool = False
    swipe_fingers: int = 3
    sustain_min: int = 1
    swipe_mm: int = 12
    swipe_ratio_x10: int = 20
    settle_ms: int = 40
    swipe_window_10ms: int = 60
    swipe_repeat_mm: int = 0
    pinch_start_x2: int = 8
    pinch_step_x2: int = 6
    keycodes: tuple = (0x094F, 0x0950, 0x082B, 0x0807, 0x01D9, 0x01DA)

    @property
    def firmware_swipe(self): return self.swipe_owner == "firmware"
    @property
    def firmware_pinch(self): return self.pinch_owner == "firmware"
    @property
    def chip_scroll(self): return self.scroll_owner == "chip"

    def problems(self):
        out = []
        for name, (_, on, off) in OWNER_BITS.items():
            if getattr(self, name) not in (on, off):
                out.append(f"{name} must be {off} or {on}")
        for name, (lo, hi) in RANGES.items():
            if not lo <= getattr(self, name) <= hi:
                out.append(f"{name} must be {lo}-{hi}")
        if self.sustain_min > self.swipe_fingers:
            out.append("sustain_min must not exceed swipe_fingers")
        if self.swipe_repeat_mm and self.swipe_repeat_mm < 4:
            out.append("swipe_repeat_mm must be 0 (fire once) or 4-40")
        if len(self.keycodes) != 6 or any(not 0 <= k <= 0xFFFF for k in self.keycodes):
            out.append("keycodes must be six 16-bit values")
        return out

    def encode(self):
        if self.problems():
            raise ValueError("; ".join(self.problems()))
        owners = sum(bit for name, (bit, on, _) in OWNER_BITS.items() if getattr(self, name) == on)
        owners |= sum(bit for name, bit in OPTION_BITS.items() if getattr(self, name))
        return struct.pack("<9B6H", owners, self.swipe_fingers | self.sustain_min << 4, self.swipe_mm,
                           self.swipe_ratio_x10, self.settle_ms, self.swipe_window_10ms,
                           self.swipe_repeat_mm, self.pinch_start_x2, self.pinch_step_x2, *self.keycodes)

    @classmethod
    def decode(cls, payload):
        if len(payload) < SIZE:
            raise ValueError("Touch gesture payload is 21 bytes")
        v = struct.unpack_from("<9B6H", payload)
        owners = v[0]
        kw = {name: (on if owners & bit else off) for name, (bit, on, off) in OWNER_BITS.items()}
        kw.update({name: bool(owners & bit) for name, bit in OPTION_BITS.items()})
        return cls(**kw, swipe_fingers=v[1] & 15, sustain_min=v[1] >> 4, swipe_mm=v[2], swipe_ratio_x10=v[3],
                   settle_ms=v[4], swipe_window_10ms=v[5], swipe_repeat_mm=v[6], pinch_start_x2=v[7],
                   pinch_step_x2=v[8], keycodes=tuple(v[9:15]))

    def with_settings(self, **changes):
        """Copy with named changes; keycodes may be given by output name."""
        keycodes = list(self.keycodes)
        for i, name in enumerate(OUTPUTS):
            if name in changes:
                keycodes[i] = int(changes.pop(name))
        return replace(self, keycodes=tuple(keycodes), **changes)

    def summary(self):
        return (f"scroll {self.scroll_owner}, swipe {self.swipe_owner} ({self.swipe_fingers} fingers), "
                f"pinch {self.pinch_owner}{', across pads' if self.span_pads else ''}")


def field_names():
    return [f.name for f in fields(GestureConfig) if f.name != "keycodes"] + list(OUTPUTS)


def parse_value(name, text):
    if name in OWNER_BITS:
        return text.lower()
    if name in OPTION_BITS:
        return text.lower() in ("1", "true", "yes", "on")
    return int(text, 0)


# ---- the wire, through transport.StreamClient.sval()

def get(client):
    reply = client.sval(COMMAND, bytes((GET,)))
    if reply is None:
        return None  # the firmware has no 0x2E: every gesture belongs to Windows
    if reply[0] != GET or reply[1]:
        raise RuntimeError(f"Touch gestures GET: {STATUS_TEXT.get(reply[1], reply[1])}")
    if reply[2] != VERSION or reply[3] != SIZE:
        raise RuntimeError(f"Touch gestures payload v{reply[2]} ({reply[3]} bytes) is not v1")
    return GestureConfig.decode(reply[4:4 + SIZE])


def put(client, config):
    reply = client.sval(COMMAND, bytes((SET, VERSION, SIZE)) + config.encode())
    if reply is None or reply[0] != SET or reply[1]:
        raise RuntimeError(f"Touch gestures SET: {STATUS_TEXT.get(reply[1], reply[1]) if reply else 'not supported'}")


def defaults(client):
    reply = client.sval(COMMAND, bytes((DEFAULTS,)))
    if reply is None or reply[1]:
        raise RuntimeError("Touch gestures DEFAULTS failed")
    return GestureConfig.decode(reply[2:2 + SIZE])


def status(client):
    reply = client.sval(COMMAND, bytes((STATUS,)))
    if reply is None or reply[1]:
        raise RuntimeError("Touch gestures STATUS failed")
    lease, claim, last, count, cancels, left, right = struct.unpack_from("<BBBHHBB", reply, 2)
    modes = {0: "normal", 1: "single-pad lease", 2: "unified"}
    return {"mode": modes.get(lease & 0x7F, lease & 0x7F) + (" (handing back)" if lease & 0x80 else ""),
            "claimed": bool(claim), "last_gesture": GESTURES[last] if last < len(GESTURES) else last,
            "outputs": count, "claims": cancels,
            "contacts_left": None if left == 0xFF else left, "contacts_right": None if right == 0xFF else right}
