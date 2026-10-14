import ctypes
import json
import tempfile
import unittest
from pathlib import Path

import gesture_config as gc
from transport import Assembler, StreamClient
from test_multitouch import frame_bytes
from windows_settings import SessionSettings, TouchpadParameters

# The firmware's defaults, byte for byte (touch_gestures.c tg_defaults).
DEFAULT_PAYLOAD = bytes((0x08, 0x13, 12, 20, 40, 60, 0, 8, 6)) + bytes.fromhex("4f095009 2b080708 d901da01".replace(" ", ""))


class PayloadTests(unittest.TestCase):
    def test_defaults_match_firmware(self):
        self.assertEqual(gc.GestureConfig().encode(), DEFAULT_PAYLOAD)
        self.assertEqual(gc.GestureConfig.decode(DEFAULT_PAYLOAD), gc.GestureConfig())

    def test_round_trip_and_named_changes(self):
        c = gc.GestureConfig().with_settings(scroll_owner="chip", swipe_owner="firmware", sustain_min=2,
                                             swipe_repeat_mm=8, swipe_up=0x7700)
        raw = c.encode()
        self.assertEqual(raw[0], 0x0B)
        self.assertEqual(raw[1], 0x23)
        self.assertEqual(gc.GestureConfig.decode(raw), c)
        self.assertTrue(c.chip_scroll and c.firmware_swipe and not c.firmware_pinch)

    def test_ranges(self):
        for changes in ({"swipe_fingers": 6}, {"sustain_min": 4}, {"swipe_repeat_mm": 2},
                        {"pinch_owner": "chip"}, {"swipe_window_10ms": 5}):
            with self.assertRaises(ValueError):
                gc.GestureConfig().with_settings(**changes).encode()

    def test_cli_value_parsing(self):
        self.assertEqual(gc.parse_value("swipe_left", "0x094F"), 0x094F)
        self.assertEqual(gc.parse_value("span_pads", "off"), False)
        self.assertEqual(gc.parse_value("pinch_owner", "Firmware"), "firmware")


class FakeSval:
    """StreamClient.sval stand-in that runs the firmware's sub-commands on a dict."""
    def __init__(self, supported=True):
        self.payload, self.supported, self.sent = DEFAULT_PAYLOAD, supported, []

    def sval(self, command, payload=b""):
        assert command == gc.COMMAND
        self.sent.append(payload)
        if not self.supported:
            return None
        sub = payload[0]
        if sub == gc.GET:
            return bytes((0, 0, 1, 21)) + self.payload
        if sub == gc.SET:
            self.payload = payload[3:24]
            return bytes((1, 0))
        if sub == gc.STATUS:
            return bytes((3, 0, 2, 1, 1, 3, 0, 1, 0, 0, 0xFF))
        raise AssertionError(sub)


class WireTests(unittest.TestCase):
    def test_get_set_status(self):
        client = FakeSval()
        self.assertEqual(gc.get(client), gc.GestureConfig())
        gc.put(client, gc.GestureConfig(pinch_owner="firmware"))
        self.assertEqual(client.sent[-1][:3], bytes((1, 1, 21)))
        self.assertTrue(gc.get(client).firmware_pinch)
        s = gc.status(client)
        self.assertEqual((s["mode"], s["claimed"], s["last_gesture"], s["outputs"], s["claims"]), ("unified", True, "swipe_left", 3, 1))
        self.assertEqual((s["contacts_left"], s["contacts_right"]), (0, None))

    def test_old_firmware_means_windows_owns_everything(self):
        self.assertIsNone(gc.get(FakeSval(supported=False)))


class Dispatcher:
    """Answers wrapped requests like the firmware would."""
    def __init__(self):
        self.requests, self.next_id = [], 100

    def exchange(self, request, accept, timeout=.25):
        self.requests.append(request)
        reply = bytearray(64)
        if request[1:5] == bytes(4):  # bootstrap
            reply[:25] = request[:25]; self.next_id += 1
            reply[25:29] = self.next_id.to_bytes(4, "little")
        elif request[5] == 0xFC:
            reply[:5] = request[:5]; reply[5] = 0xFC; reply[6] = 0x80 | request[6]
            reply[9] = 0x08 | 0x20; reply[10:12] = (7).to_bytes(2, "little")
        elif request[5] == 0xDF:
            reply[:5] = request[:5]; reply[5] = 0xDF; reply[6] = 0xFF  # unknown command
        return accept(bytes(reply[:32]))


class RebindTests(unittest.TestCase):
    def test_rebind_sends_old_id_and_token_and_keeps_old_frames(self):
        d = Dispatcher(); client = StreamClient(d)
        client.bootstrap(); first = client.client
        self.assertEqual(client.command(1, 2)["ownership"] & 0x20, 0x20)
        assembler = Assembler(client.client, client.token)
        client.rebind()
        request = d.requests[-1]
        self.assertNotEqual(client.client, first)
        self.assertEqual(request[1:5], client.client.to_bytes(4, "little"))
        self.assertEqual(request[5:8], bytes((0xFC, 2, 0)))
        self.assertEqual(request[8:10], (7).to_bytes(2, "little"))
        self.assertEqual(request[10:14], first.to_bytes(4, "little"))
        assembler.rebind(client.client)
        for owner in (first, client.client):
            packet = b"\xdd" + owner.to_bytes(4, "little") + b"\xfc\x41" + (7).to_bytes(2, "little") + frame_bytes()[1:]
            self.assertIsNotNone(assembler.accept(packet))
        self.assertIsNone(gc.get(client))  # this fake firmware has no 0x2E


class FakeApi:
    def __init__(self, **values):
        self.p = TouchpadParameters(versionNumber=1, allowActiveWhenMousePresent=0, panEnabled=1, zoomEnabled=1, cursorSpeed=12)
        for k, v in values.items():
            setattr(self.p, k, v)
        self.writes = 0

    def read(self):
        copy = TouchpadParameters()
        ctypes.memmove(ctypes.byref(copy), ctypes.byref(self.p), ctypes.sizeof(copy))
        return copy

    def write(self, p):
        self.p = p; self.writes += 1


class WindowsSettingsTests(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.backup = Path(self.dir.name) / "backup.json"

    def tearDown(self):
        self.dir.cleanup()

    def test_struct_is_the_windows_size(self):
        self.assertEqual(ctypes.sizeof(TouchpadParameters), 44)

    def test_owners_drive_pan_and_zoom_and_restore(self):
        api = FakeApi(); s = SessionSettings(api, self.backup, log=lambda m: None)
        s.apply(gc.GestureConfig(scroll_owner="chip", pinch_owner="firmware"))
        self.assertEqual((api.p.allowActiveWhenMousePresent, api.p.panEnabled, api.p.zoomEnabled), (1, 0, 0))
        self.assertEqual(api.p.cursorSpeed, 12)  # untouched
        self.assertEqual(json.loads(self.backup.read_text()), {"allowActiveWhenMousePresent": 0, "panEnabled": 1, "zoomEnabled": 1})
        s.apply(gc.GestureConfig())  # owners back to Windows mid-session; the backup keeps the originals
        self.assertEqual((api.p.panEnabled, api.p.zoomEnabled), (1, 1))
        writes = api.writes
        s.apply(gc.GestureConfig()); self.assertEqual(api.writes, writes)  # nothing changed, nothing written
        s.restore()
        self.assertEqual((api.p.allowActiveWhenMousePresent, api.p.panEnabled, api.p.zoomEnabled), (0, 1, 1))
        self.assertFalse(self.backup.exists())

    def test_crash_backup_is_restored_at_next_start(self):
        api = FakeApi(); s = SessionSettings(api, self.backup, log=lambda m: None)
        s.apply(gc.GestureConfig(scroll_owner="chip"))
        # The process dies here. The next session repairs it before anything else.
        SessionSettings(api, self.backup, log=lambda m: None).recover()
        self.assertEqual((api.p.allowActiveWhenMousePresent, api.p.panEnabled), (0, 1))
        self.assertFalse(self.backup.exists())

    def test_old_firmware_keeps_windows_gestures_on(self):
        api = FakeApi(panEnabled=0, zoomEnabled=0); s = SessionSettings(api, self.backup, log=lambda m: None)
        self.assertEqual(s.apply(None), {"allowActiveWhenMousePresent": 1, "panEnabled": 1, "zoomEnabled": 1})


if __name__ == "__main__":
    unittest.main()
