"""Instrumentation tests; hardware behavior is exercised by tools/keytest.py."""
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("keytest", ROOT / "keyboards/svalboard/tools/keytest.py")
keytest = importlib.util.module_from_spec(spec)
spec.loader.exec_module(keytest)

STUB = r'''
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#define MATRIX_ROWS 10
#define MATRIX_COLS 6
#define DYNAMIC_KEYMAP_LAYER_COUNT 16
#define KEYBOARD_REPORT_KEYS 6
#define NKRO_REPORT_BITS 30
#define SVAL_KEYTEST
#define id_custom_set_value 7
#define id_custom_get_value 8
typedef uint8_t matrix_row_t;
typedef uint32_t layer_state_t;
typedef struct { uint8_t mods, reserved, keys[6]; } report_keyboard_t;
typedef struct { uint8_t report_id, mods, bits[30]; } report_nkro_t;
typedef struct { uint8_t buttons; int16_t x, y, h, v; } report_mouse_t;
typedef struct { uint8_t report_id; uint16_t usage; } report_extra_t;
typedef struct {
 uint8_t (*keyboard_leds)(void);
 void (*send_keyboard)(report_keyboard_t *);
 void (*send_nkro)(report_nkro_t *);
 void (*send_mouse)(report_mouse_t *);
 void (*send_extra)(report_extra_t *);
 void (*send_raw_hid)(uint8_t *, uint8_t);
} host_driver_t;
typedef struct { uint8_t row, col; bool pressed; } keyevent_t;
#define MAKE_KEYEVENT(r,c,p) ((keyevent_t){r,c,p})
static uint32_t now=10;
static layer_state_t layer_state=0, default_layer_state=1;
static matrix_row_t physical[MATRIX_ROWS];
static int desktop_reports, actions, resets;
static uint32_t action_times[128];
static host_driver_t *driver;
static bool is_keyboard_master(void) { return true; }
static uint32_t timer_read32(void) { return now; }
static uint32_t timer_elapsed32(uint32_t t) { return now-t; }
static matrix_row_t matrix_get_row(uint8_t r) { return physical[r]; }
static host_driver_t *host_get_driver(void) { return driver; }
static void host_set_driver(host_driver_t *d) { driver=d; }
static void desktop(report_keyboard_t *r) { (void)r; ++desktop_reports; }
static void raw(uint8_t *d,uint8_t n) { (void)d; (void)n; }
static void clear_keyboard(void) { report_keyboard_t r={0}; driver->send_keyboard(&r); }
static void action_exec(keyevent_t e) {
 action_times[actions++]=now;
 report_keyboard_t r={.keys={e.pressed ? 4 : 0}};
 driver->send_keyboard(&r);
}
static void reset_keyboard(void) { ++resets; }
static void soft_reset_keyboard(void) { ++resets; }
static void layer_clear(void) { layer_state=0; }
static void default_layer_set(layer_state_t s) { default_layer_state=s; }
static uint8_t get_mods(void) { return 0; }
static uint8_t get_weak_mods(void) { return 0; }
'''

HARNESS = r'''
#include <assert.h>
static uint8_t packet[32];
static void command(uint8_t op, const uint8_t *args, size_t n) {
 memset(packet,0xA5,sizeof(packet)); packet[0]=op==INFO ? 8 : 7;packet[1]=0x54;packet[2]=op;
 memset(packet+3,0,23); if(n)memcpy(packet+3,args,n);
 keytest_command(packet,26);
 // Wrapped VIA has only 26 usable bytes: never write the stale tail.
 for(int i=26;i<32;i++)assert(packet[i]==0xA5);
}
static void event(uint16_t delay,uint8_t row,uint8_t col,uint8_t down) {
 uint8_t a[]={delay,delay>>8,row,col,down};command(ENQUEUE,a,sizeof(a));
}
int main(void) {
 host_driver_t real={.send_keyboard=desktop,.send_raw_hid=raw};driver=&real;
 assert(should_process_keypress());
 event(0,0,0,1);assert(packet[3]==INACTIVE);
 physical[0]=1;command(BEGIN,(uint8_t *)"TEST",4);assert(packet[3]==BUSY);
 physical[0]=0;command(BEGIN,(uint8_t *)"TEST",4);assert(!packet[3]);
 assert(desktop_reports==1 && !should_process_keypress() && driver->send_raw_hid==raw);
 event(0,10,0,1);assert(packet[3]==INVALID);
 event(0,0,6,1);assert(packet[3]==INVALID);
 event(0,0,0,2);assert(packet[3]==INVALID);
 event(0,0,0,0);assert(packet[3]==INVALID);
 event(0,0,0,1);assert(!packet[3]);
 event(0,0,0,1);assert(packet[3]==INVALID);
 event(40,0,0,0);assert(!packet[3]);
 command(RUN,NULL,0);assert(!packet[3]);keytest_task();
 assert(actions==1 && held[0]==1);
 now+=39;keytest_task();assert(actions==1);
 now++;keytest_task();assert(actions==2 && held[0]==0 && !running);
 assert(action_times[1]-action_times[0]==40 && desktop_reports==1);
 assert(reports[0].kind==INPUT && reports[1].kind==KEYBOARD && reports[1].bytes[1]==4);
 assert(reports[3].bytes[1]==0);
 // Full 31-byte NKRO records can be reconstructed through the short wrapper.
 report_nkro_t n={.mods=2};for(int i=0;i<30;i++)n.bits[i]=i;
 uint32_t seq=next_report;driver->send_nkro(&n);
 for(uint8_t offset=0;offset<31;offset+=11){
  uint8_t a[5];put32(a,seq);a[4]=offset;command(READ,a,5);
  assert(!packet[3] && packet[4]==NKRO && packet[5]==31 && packet[14]==offset);
  for(int j=0;j<11 && offset+j<31;j++)assert(packet[15+j]==(offset+j ? offset+j-1 : 2));
 }
 uint8_t ack[4];put32(ack,next_report);command(ACK,ack,4);assert(!packet[3] && first_report==next_report);
 uint8_t layer=3;command(SELECT_LAYER,&layer,1);assert(!packet[3] && default_layer_state==8);
 uint8_t short_packet[4]={7,0x54,ENQUEUE,0};keytest_command(short_packet,4);assert(short_packet[3]==INVALID);
 // Signed deadline comparison remains correct across timer wrap.
 now=UINT32_MAX-10;event(20,0,0,1);event(1,0,0,0);command(RUN,NULL,0);
 int before=actions;now=8;keytest_task();assert(actions==before);
 now=9;keytest_task();assert(actions==before+1);now=10;keytest_task();assert(actions==before+2);
 command(CLEAR,NULL,0);assert(!packet[3] && !next_report);
 for(int i=0;i<32;i++){event(0,0,0,!(i&1));assert(!packet[3]);}
 event(0,0,0,1);assert(packet[3]==FULL);
 command(RUN,NULL,0);assert(!packet[3]);
 command(CLEAR,NULL,0);assert(packet[3]==BUSY);
 for(int i=0;i<32;i++)keytest_task();
 assert(!running && !count && next_report==64);
 driver->send_nkro(&n);assert(lost_reports==1 && first_report==1);
 uint8_t old[5]={0};command(READ,old,5);assert(packet[3]==MISSING);
 // Releasing on disconnect also stays isolated; only reboot ends capture.
 event(0,1,1,1);command(RUN,NULL,0);keytest_task();assert(held[1]==2);
 now+=WATCHDOG_MS;keytest_task();
 assert(aborted && !held[1] && !count && !running && desktop_reports==1);
 event(0,0,0,1);assert(packet[3]==ABORTED);
 command(REBOOT,NULL,0);assert(!packet[3] && resets==0);
 now+=99;keytest_task();assert(!resets);now++;keytest_task();assert(resets==1);
 return 0;
}
'''


class InstrumentationTests(unittest.TestCase):
    def test_production_firmware_transport_and_scheduler(self):
        with tempfile.TemporaryDirectory() as directory:
            p = Path(directory)
            (p / "quantum.h").write_text(STUB)
            for name in ("host.h", "via.h", "dynamic_keymap.h"):
                (p / name).write_text('#include "quantum.h"\n')
            (p / "test.c").write_text(f'#include "{ROOT}/keyboards/svalboard/keytest.c"\n' + HARNESS)
            subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-fsanitize=undefined", "-I", str(p), str(p / "test.c"), "-o", str(p / "test")], check=True)
            subprocess.run([str(p / "test")], check=True)

    def test_decode_boot_and_nkro_and_assert_release(self):
        down = keytest.decode_record(0, 1, 10, bytes([0, 4, 0, 0, 0, 0, 0]))
        up = keytest.decode_record(1, 2, 20, bytes(31))
        keytest.expect_tap([down, up], 4)
        with self.assertRaises(AssertionError):
            keytest.expect_tap([down], 4)
        bits = bytearray(31)
        bits[0], bits[1], bits[-1] = 2, 1 << 4, 1 << 7
        nkro = keytest.decode_record(2, 2, 25, bits)
        self.assertEqual(nkro["keys"], [4, 239])
        self.assertEqual(nkro["mods"], 2)

    def test_binding_roundtrip_and_rollback(self):
        class FakeDevice:
            serial, handle = "test", True

            def __init__(self, lose_on_reboot=False):
                self.value, self.active, self.reboots = 5, False, 0
                self.lose_on_reboot = lose_on_reboot

            def info(self):
                return dict(rows=10, cols=6, layers=16, active=self.active)

            def keymap(self, l, r, c, value=None):
                if value is not None:
                    self.value = value
                return self.value

            def begin(self):
                self.active = True

            def command(self, op, args=b""):
                pass

            def events(self, events):
                pass

            def collect(self, settle_ms):
                return [keytest.decode_record(0, 1, 0, bytes([0, self.value, 0, 0, 0, 0, 0])), keytest.decode_record(1, 1, 30, bytes(7))]

            def reboot(self):
                self.active = False
                self.reboots += 1
                if self.lose_on_reboot and self.reboots == 1:
                    self.value = 6

        with tempfile.TemporaryDirectory() as directory:
            args = SimpleNamespace(row=0, col=0, layer=0, keycode=4, backup=str(Path(directory) / "original.json"))
            device = FakeDevice()
            self.assertTrue(keytest.verify_binding(device, args)["restored"])
            self.assertEqual((device.value, device.reboots), (5, 2))
            # A lost setting on reboot is a test failure, never just readback success.
            args.backup = str(Path(directory) / "failure.json")
            device = FakeDevice(True)
            with self.assertRaisesRegex(AssertionError, "survive reboot"):
                keytest.verify_binding(device, args)
            self.assertEqual((device.value, device.reboots), (5, 2))
            self.assertTrue(Path(args.backup).exists())

    def test_sval_commands_use_required_client_wrapper(self):
        device = object.__new__(keytest.Device)
        requests = []

        def exchange(request):
            requests.append(bytes(request))
            packet = bytes(request).ljust(32, b"\0")
            if packet[1:5] == bytes(4):
                return packet[:25] + bytes([42, 0, 0, 0, 120, 0, 0])
            return packet

        device._exchange = exchange
        reply = device.exchange([0xDF, 4, 0])
        self.assertEqual(reply[:3], bytes([0xDF, 4, 0]))
        self.assertEqual(len(requests), 2)
        self.assertEqual(requests[1][:6], bytes([0xDD, 42, 0, 0, 0, 0xDF]))

    def test_chunked_capture_read_and_ack(self):
        import struct
        device = object.__new__(keytest.Device)
        payload = bytes([2, 16] + [0] * 28 + [128])
        acknowledgements = []
        device.info = lambda: dict(first=0, next=1, lost=0)

        def command(op, args):
            if op == keytest.ACK:
                acknowledgements.append(struct.unpack("<I", args)[0])
                return b""
            seq, offset = struct.unpack("<IB", args)
            self.assertEqual(seq, 0)
            return bytes([2, len(payload)]) + struct.pack("<IIB", 123, seq, offset) + payload[offset:offset + 11].ljust(11, b"\0")

        device.command = command
        records, cursor = device.records()
        self.assertEqual((cursor, acknowledgements), (1, [1]))
        self.assertEqual(records[0]["keys"], [4, 239])
        self.assertEqual(records[0]["mods"], 2)
        self.assertEqual(records[0]["time_ms"], 123)

    def test_older_firmware_is_only_probed_with_read(self):
        device = object.__new__(keytest.Device)
        requests = []

        def exchange(request):
            requests.append(bytes(request))
            # Older board handler echoes with zero protocol version.
            return bytes(request).ljust(32, b"\0")

        device.exchange = exchange
        with self.assertRaisesRegex(RuntimeError, "Unsupported keytest"):
            device.command(keytest.REBOOT)
        self.assertEqual(requests, [bytes([8, 0x54, 0])])

    def test_capture_overflow_is_not_a_pass(self):
        device = object.__new__(keytest.Device)
        device.info = lambda: dict(first=1, next=65, lost=1)
        with self.assertRaisesRegex(RuntimeError, "overflow"):
            device.records()


if __name__ == "__main__":
    unittest.main()
