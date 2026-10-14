"""Contact framing and a single HID reader shared by commands and streaming."""
from dataclasses import dataclass
import queue
import secrets
import struct
import threading
import time

PROTOCOL = 0xFC
FRAME_HEADER = struct.Struct("<BBBBHHII")
CONTACT = struct.Struct("<BHHHB")


@dataclass(frozen=True)
class Frame:
    pad: int
    flags: int
    mask: int
    sequence: int
    timestamp: int
    epoch: int
    contacts: tuple

    @classmethod
    def decode(cls, raw):
        if len(raw) != 56:
            raise ValueError("Invalid frame size")
        version, pad, flags, mask, seq, size, timestamp, epoch = FRAME_HEADER.unpack_from(raw)
        if version != 1 or size != 56 or pad > 1 or mask & ~31:
            raise ValueError("Invalid frame header")
        contacts = tuple(CONTACT.unpack_from(raw, 16 + 8 * i) for i in range(5))
        active = [c for i, c in enumerate(contacts) if mask & (1 << i)]
        if len({c[0] for c in active}) != len(active):
            raise ValueError("Duplicate lifetime IDs")
        if any(not c[0] or c[1] > 2048 or c[2] > 1792 or not c[3] for c in active):
            raise ValueError("Invalid active contact")
        return cls(pad, flags, mask, seq, timestamp, epoch, contacts)

    def himetric(self):
        return {c[0]: (round(c[1] * 4300 / 2048), round(c[2] * 4000 / 1792))
                for i, c in enumerate(self.contacts) if self.mask & (1 << i)}


class Assembler:
    def __init__(self, client, token, clock=time.monotonic):
        self.client, self.token, self.clock = client, token, clock
        self.previous = None  # the wrapper ID before a rebind, still accepted
        self.key, self.parts, self.started = None, {}, 0
        self.incomplete = 0

    def rebind(self, client):
        """A renew moved the lease to a fresh wrapper ID: frames already queued
        under the old one stay valid, so the stream carries on unbroken."""
        self.previous, self.client = self.client, client

    def accept(self, data):
        if len(data) not in (32, 64):
            raise ValueError("Invalid report size")
        if data[:1] != b"\xdd" or int.from_bytes(data[1:5], "little") not in (self.client, self.previous):
            return None
        if data[5] != PROTOCOL or data[6] not in (0x40, 0x41):
            return None
        if data[6] == 0x41:
            if len(data) != 64:
                raise ValueError("Compact frame requires a 64-byte report")
            if int.from_bytes(data[7:9], "little") != self.token:
                return None
            return Frame.decode(b"\x01" + bytes(data[9:64]))
        token, seq, index, length = struct.unpack_from("<HHBB", data, 7)
        if token != self.token:
            return None
        if index > 2 or length != (18 if index == 2 else 19):
            raise ValueError("Invalid fragment")
        now = self.clock()
        key = token, seq
        if key != self.key or now - self.started > .05:
            if self.parts:
                self.incomplete += 1
            self.key, self.parts, self.started = key, {}, now
        payload = bytes(data[13:13 + length])
        if index in self.parts and self.parts[index] != payload:
            self.parts = {}
            raise ValueError("Conflicting fragment")
        self.parts[index] = payload
        if len(self.parts) != 3:
            return None
        raw = b"".join(self.parts[i] for i in range(3))
        self.parts = {}
        frame = Frame.decode(raw)
        if frame.sequence != seq:
            raise ValueError("Sequence differs from envelope")
        return frame


@dataclass
class Request:
    packet: bytes
    accept: object
    timeout: float
    done: object
    result: object = None
    error: object = None


class HidDispatcher:
    """Owns all I/O. Stream callback must only enqueue, never render or block.

    Command matchers execute on this thread. Only one command is outstanding;
    unrelated reports are ignored without losing contact fragments. Handles are
    owned and closed by the caller after close() joins this thread.
    """
    def __init__(self, device, on_stream=lambda packet: None, report_size=32):
        if report_size not in (32, 64):
            raise ValueError("Report size must be 32 or 64")
        self.report_size = report_size
        self.device, self.on_stream = device, on_stream
        self.requests = queue.Queue(maxsize=32)
        self.stop = threading.Event()
        self.failure = None
        self.thread = threading.Thread(target=self._run, name="Keybard HID dispatcher", daemon=True)
        self.thread.start()

    def exchange(self, request, accept, timeout=.25):
        if not 1 <= len(request) <= 32:
            raise ValueError("Invalid request size")
        if self.failure or self.stop.is_set():
            raise OSError(f"HID dispatcher stopped: {self.failure}")
        item = Request(b"\0" + request.ljust(self.report_size, b"\0"), accept, timeout, threading.Event())
        self.requests.put_nowait(item)
        while not item.done.wait(.05):
            if self.failure or self.stop.is_set():
                raise OSError(f"HID dispatcher stopped: {self.failure}")
        if item.error:
            raise item.error
        return item.result

    def _run(self):
        active = None
        try:
            while not self.stop.is_set():
                if active is None:
                    try:
                        active = self.requests.get_nowait()
                    except queue.Empty:
                        pass
                    if active:
                        if self.device.write(active.packet) != self.report_size + 1:
                            raise OSError("Short HID write")
                        deadline = time.monotonic() + active.timeout
                packet = bytes(self.device.read(self.report_size, 2))
                if packet:
                    if len(packet) != self.report_size:
                        raise OSError("Malformed HID packet size")
                    # This experimental protocol still uses 32 logical bytes.
                    # Pad writes/read full physical reports on newer firmware.
                    if packet[0] == 0xDD and packet[5] == PROTOCOL and packet[6] in (0x40, 0x41):
                        self.on_stream(packet if packet[6] == 0x41 else packet[:32])
                    elif active:
                        try:
                            result = active.accept(packet[:32])
                            if result is not None:
                                active.result = result
                                active.done.set()
                                active = None
                        except Exception as exc:
                            active.error = exc
                            active.done.set()
                            active = None
                if active and time.monotonic() >= deadline:
                    active.error = TimeoutError("HID command timed out")
                    active.done.set()
                    active = None
        except Exception as exc:
            self.failure = exc
        finally:
            if active:
                active.error = OSError(f"HID dispatcher stopped: {self.failure}")
                active.done.set()

    def close(self):
        self.stop.set()
        self.thread.join(1)
        if self.thread.is_alive():
            raise OSError("HID read did not stop; do not reuse the handle")


class StreamClient:
    def __init__(self, dispatcher):
        self.dispatcher, self.client, self.token = dispatcher, 0, 0

    def bootstrap(self):
        prefix = b"\xdd" + bytes(4) + secrets.token_bytes(20)
        def accept(reply):
            if reply[:25] != prefix:
                return None
            return int.from_bytes(reply[25:29], "little")
        self.client = self.dispatcher.exchange(prefix, accept)
        if self.client in (0, 0xffffffff):
            raise ValueError("Invalid client ID")

    def command(self, op, mode=0, extra=b""):
        if not self.client:
            self.bootstrap()
        prefix = b"\xdd" + self.client.to_bytes(4, "little")
        def accept(reply):
            if reply[:5] != prefix:
                return None
            if reply[5] == 0xff:
                raise RuntimeError(f"Client wrapper rejected stream: {reply[6]}")
            if reply[5] != PROTOCOL or reply[6] != (0x80 | op):
                return None
            if reply[7]:
                raise RuntimeError(f"Stream command {op} rejected: {reply[7]}")
            return reply
        reply = self.dispatcher.exchange(prefix + bytes((PROTOCOL, op, mode)) + self.token.to_bytes(2, "little") + extra, accept)
        self.token = int.from_bytes(reply[10:12], "little")
        return {"version": reply[8], "ownership": reply[9], "token": self.token,
                "x_max": int.from_bytes(reply[12:14], "little"), "y_max": int.from_bytes(reply[14:16], "little"),
                "width_mm": reply[16], "height_mm": reply[17], "max_contacts": reply[18],
                "samples": int.from_bytes(reply[19:23], "little"),
                "errors": int.from_bytes(reply[23:27], "little"), "dropped": int.from_bytes(reply[27:31], "little")}

    def rebind(self):
        """Renew the lease under a fresh wrapper ID (IDs expire after two
        minutes). Same token; the old ID proves the lease is ours. No STOP and
        START, so a gesture in progress is not cut off."""
        old = self.client
        self.bootstrap()
        try:
            return self.command(2, extra=old.to_bytes(4, "little"))
        except Exception:
            self.client = old
            raise

    def sval(self, command, payload=b""):
        """One wrapped Sval (0xDF) request. Returns the reply bytes after
        DF <command>, or None when the firmware doesn't know the command."""
        prefix = b"\xdd" + self.client.to_bytes(4, "little")
        def accept(reply):
            if reply[:5] != prefix or reply[5] != 0xDF:
                return None
            if reply[6] == 0xFF:
                return b""
            if reply[6] != command:
                return None
            return bytes(reply[7:])
        reply = self.dispatcher.exchange(prefix + bytes((0xDF, command)) + payload, accept)
        return reply or None

    def diagnostics(self):
        prefix = b"\xdd" + self.client.to_bytes(4, "little")
        def accept(reply):
            if reply[:5] == prefix and reply[5:7] == bytes((PROTOCOL, 0x84)):
                return reply
            return None
        reply = self.dispatcher.exchange(prefix + bytes((PROTOCOL, 4)), accept)
        return dict(zip(("read_timeouts", "read_errors", "end_timeouts", "end_errors", "max_read_ms"),
                        struct.unpack_from("<5I", reply, 8)))

    def retry_diagnostics(self):
        prefix = b"\xdd" + self.client.to_bytes(4, "little")
        def accept(reply):
            if reply[:5] == prefix and reply[5:7] == bytes((PROTOCOL, 0x88)):
                return reply
            return None
        reply = self.dispatcher.exchange(prefix + bytes((PROTOCOL, 8)), accept)
        result = dict(zip(("acquisition_faults", "nack_faults", "retry_attempts",
                         "retry_recovered", "nonack_faults", "last_i2c_flags"),
                        struct.unpack_from("<6I", reply, 8)))
        def accept_abort(reply):
            if reply[:5] == prefix and reply[5:7] == bytes((PROTOCOL, 0x89)):
                return reply
            return None
        reply = self.dispatcher.exchange(prefix + bytes((PROTOCOL, 9)), accept_abort)
        result["rp_abort_source"] = struct.unpack_from("<I", reply, 8)[0]
        return result

    def characterize(self, op, maximum=5, period=10):
        if op not in (5, 6, 7):
            raise ValueError("Invalid characterization operation")
        prefix = b"\xdd" + self.client.to_bytes(4, "little")
        def accept(reply):
            if reply[:5] != prefix or reply[5:7] != bytes((PROTOCOL, 0x80 | op)):
                return None
            if reply[7]:
                raise RuntimeError(f"Characterization command rejected: {reply[7]}")
            return reply
        request = prefix + bytes((PROTOCOL, op, 0)) + struct.pack("<HBH", self.token, maximum, period)
        reply = self.dispatcher.exchange(request, accept)
        if op == 6:
            return dict(zip(("sensor_samples", "cycle_sum_ms", "cycle_max_ms", "rate_missed", "too_many", "acquisition_sum_ms"),
                            struct.unpack_from("<6I", reply, 8)))
        return {"maximum": reply[8], "period_ms": int.from_bytes(reply[9:11], "little"), "restore_pending": bool(reply[11])}


class FrameGuard:
    """Cancel on discontinuity/staleness; require all-up before restarting."""
    def __init__(self, backend, require_owner=True, clock=time.monotonic):
        self.backend, self.require_owner, self.clock = backend, require_owner, clock
        self.last, self.key, self.sequence = 0, None, None
        self.ready = False
        self.cancellations = 0

    def cancel(self):
        if hasattr(self.backend, "cancel"):
            self.backend.cancel()
        else:
            self.backend.frame({})
        self.ready = False
        self.cancellations += 1

    def feed(self, frame):
        key = frame.pad, frame.epoch
        gap = self.sequence is not None and ((frame.sequence - self.sequence) & 0xffff) != 1
        if key != self.key or gap or frame.flags & 0x7f or (self.require_owner and not frame.flags & 0x80):
            self.cancel()
        self.key, self.sequence, self.last = key, frame.sequence, self.clock()
        if frame.flags & 0x7f or (self.require_owner and not frame.flags & 0x80):
            return
        if not frame.mask:
            self.ready = True
        if self.ready:
            self.backend.frame(frame.himetric())

    def tick(self):
        if self.ready and self.clock() - self.last > .05:
            self.cancel()
