import importlib.util
from pathlib import Path
import queue
import struct
import sys
import threading
import time
import unittest

from transport import Assembler, Frame, FrameGuard, HidDispatcher, CONTACT, FRAME_HEADER


def frame_bytes(seq=1, epoch=1, mask=1, flags=0x80, contact_id=1):
    header = FRAME_HEADER.pack(1, 1, flags, mask, seq, 56, seq * 10000, epoch)
    return header + b"".join(CONTACT.pack(contact_id + i, i * 200, 300, 100, 2) for i in range(5))


def fragments(raw, client=123, token=7):
    seq = struct.unpack_from("<H", raw, 4)[0]
    packets = []
    for i in range(3):
        payload = raw[i * 19:(i + 1) * 19]
        header = b"\xdd" + struct.pack("<I", client) + bytes((0xfc, 0x40))
        packets.append((header + struct.pack("<HHBB", token, seq, i, len(payload)) + payload).ljust(32, b"\0"))
    return packets


class FakeHid:
    def __init__(self):
        self.incoming = queue.Queue()
        self.writes = []
        self.read_threads = set()

    def write(self, packet):
        self.writes.append(packet)
        for p in fragments(frame_bytes()):
            self.incoming.put(p)
        self.incoming.put(bytes(packet[1:]))
        return len(packet)

    def read(self, size, timeout):
        self.read_threads.add(threading.get_ident())
        try:
            return self.incoming.get(timeout=timeout / 1000)
        except queue.Empty:
            return b""


class Sink:
    def __init__(self):
        self.frames = []

    def frame(self, contacts):
        self.frames.append(contacts)


class FramingTests(unittest.TestCase):
    def test_sparse_slots_zero_x_and_endianness(self):
        frame = Frame.decode(frame_bytes(mask=17))
        self.assertEqual(set(frame.himetric()), {1, 5})
        self.assertEqual(frame.contacts[0][1], 0)
        self.assertEqual(frame.contacts[4][1], 800)

    def test_reordered_fragments_and_other_client(self):
        assembler = Assembler(123, 7)
        self.assertIsNone(assembler.accept(fragments(frame_bytes(), client=99)[0]))
        p = fragments(frame_bytes())
        self.assertIsNone(assembler.accept(p[2]))
        self.assertIsNone(assembler.accept(p[0]))
        self.assertEqual(assembler.accept(p[1]).sequence, 1)

    def test_incomplete_frame_never_combines_with_next(self):
        assembler = Assembler(123, 7)
        old, new = fragments(frame_bytes()), fragments(frame_bytes(seq=2))
        assembler.accept(old[0])
        for p in new[:-1]:
            self.assertIsNone(assembler.accept(p))
        self.assertEqual(assembler.accept(new[-1]).sequence, 2)
        self.assertEqual(assembler.incomplete, 1)

    def test_conflicting_duplicate_is_rejected(self):
        assembler = Assembler(123, 7)
        p = fragments(frame_bytes())[0]
        assembler.accept(p)
        bad = bytearray(p); bad[20] ^= 1
        with self.assertRaises(ValueError):
            assembler.accept(bad)

    def test_bad_contact_and_fragment_lengths(self):
        bad = bytearray(frame_bytes()); bad[17:19] = b"\xff\xff"
        with self.assertRaises(ValueError):
            Frame.decode(bad)
        bad = bytearray(fragments(frame_bytes())[0]); bad[12] = 20
        with self.assertRaises(ValueError):
            Assembler(123, 7).accept(bad)

    def test_expired_fragments_do_not_complete(self):
        now = [0]
        assembler = Assembler(123, 7, lambda: now[0])
        parts = fragments(frame_bytes())
        assembler.accept(parts[0]); now[0] = .1
        self.assertIsNone(assembler.accept(parts[1]))
        self.assertIsNone(assembler.accept(parts[2]))


class GuardTests(unittest.TestCase):
    def setUp(self):
        self.now, self.sink = [1], Sink()
        self.guard = FrameGuard(self.sink, clock=lambda: self.now[0])

    def feed(self, **kwargs):
        self.guard.feed(Frame.decode(frame_bytes(**kwargs)))

    def test_requires_all_up_and_owner(self):
        self.feed(mask=1)
        self.assertEqual(self.sink.frames[-1], {})
        self.feed(seq=2, mask=0)
        self.feed(seq=3, mask=1)
        self.assertTrue(self.sink.frames[-1])
        self.feed(seq=4, mask=1, flags=0)
        self.assertEqual(self.sink.frames[-1], {})

    def test_gap_stale_and_epoch_release(self):
        self.feed(mask=0); self.feed(seq=2)
        self.now[0] += .06
        self.guard.tick()
        self.assertEqual(self.sink.frames[-1], {})
        self.feed(seq=3)
        self.assertFalse(self.guard.ready)
        self.feed(seq=4, mask=0); self.feed(seq=6)
        self.assertFalse(self.guard.ready)
        self.feed(seq=7, mask=0); self.feed(seq=8, epoch=2)
        self.assertFalse(self.guard.ready)

    def test_sequence_wrap(self):
        self.feed(seq=65535, mask=0); self.feed(seq=0)
        self.assertTrue(self.sink.frames[-1])


class DispatcherTests(unittest.TestCase):
    def test_commands_and_stream_share_one_reader(self):
        device, received = FakeHid(), []
        dispatcher = HidDispatcher(device, received.append)
        try:
            for i in range(10):
                request = bytes((i,))
                self.assertEqual(dispatcher.exchange(request, lambda p: p if p[0] == i else None)[0], i)
            self.assertEqual(len(received), 30)
            self.assertEqual(len(device.read_threads), 1)
        finally:
            dispatcher.close()

    def test_idle_stream_is_drained(self):
        device, arrived = FakeHid(), threading.Event()
        dispatcher = HidDispatcher(device, lambda p: arrived.set())
        try:
            device.incoming.put(fragments(frame_bytes())[0])
            self.assertTrue(arrived.wait(.5))
        finally:
            dispatcher.close()

    def test_bad_packet_fails_closed(self):
        device = FakeHid()
        dispatcher = HidDispatcher(device)
        try:
            device.incoming.put(b"short")
            dispatcher.thread.join(.5)
            self.assertIsNotNone(dispatcher.failure)
            with self.assertRaises(OSError):
                dispatcher.exchange(b"a", lambda p: p)
        finally:
            dispatcher.close()


if __name__ == "__main__":
    unittest.main()

class ReportSizeTests(unittest.TestCase):
    def test_64_byte_reports_preserve_logical_commands_and_stream(self):
        class Hid64(FakeHid):
            def write(self, packet):
                self.writes.append(packet)
                for p in fragments(frame_bytes()):
                    self.incoming.put(p.ljust(64, b'\0'))
                self.incoming.put(bytes(packet[1:]))
                return len(packet)
        device = Hid64()
        received = []
        dispatcher = HidDispatcher(device, received.append, report_size=64)
        try:
            reply = dispatcher.exchange(b'\x01', lambda p: p if p[0] == 1 else None)
            self.assertEqual(len(device.writes[0]), 65)
            self.assertEqual(reply, b'\x01' + bytes(31))
            self.assertEqual(received, fragments(frame_bytes()))
        finally:
            dispatcher.close()

class CompactFrameTests(unittest.TestCase):
    @staticmethod
    def packet(raw, client=123, token=7):
        return b'\xdd' + struct.pack('<I', client) + b'\xfc\x41' + struct.pack('<H', token) + raw[1:]

    def test_complete_five_contact_frame_is_one_64_byte_report(self):
        raw = frame_bytes(mask=31)
        packet = self.packet(raw)
        self.assertEqual(len(packet), 64)
        decoded = Assembler(123, 7).accept(packet)
        self.assertEqual(decoded, Frame.decode(raw))

    def test_compact_frame_checks_session_and_lengths(self):
        a = Assembler(123, 7)
        self.assertIsNone(a.accept(self.packet(frame_bytes(), client=999)))
        self.assertIsNone(a.accept(self.packet(frame_bytes(), token=8)))
        with self.assertRaises(ValueError): a.accept(self.packet(frame_bytes())[:32])

    def test_dispatcher_preserves_the_whole_compact_frame(self):
        packet = self.packet(frame_bytes(mask=31))
        class Hid64(FakeHid):
            def write(self, data):
                self.incoming.put(packet)
                self.incoming.put(data[1:])
                return len(data)
        got = []
        h = HidDispatcher(Hid64(), got.append, report_size=64)
        try:
            h.exchange(b'\x01', lambda p: p if p[0] == 1 else None)
            self.assertEqual(got, [packet])
        finally: h.close()
