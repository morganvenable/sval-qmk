"""Host tests compile the actual prototype C sources; no QMK toolchain required."""
import ctypes as C
from pathlib import Path
import random
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'keyboards/svalboard/pointing'
HERE = Path(__file__).resolve().parent


class Policy(C.Structure):
    _fields_ = [('mode', C.c_int), ('h', C.c_int32), ('v', C.c_int32), ('last_motion', C.c_uint16)]


class Turn(C.Structure):
    _fields_ = [('direction', C.c_int8), ('progress', C.c_int32), ('reverse', C.c_int32), ('last_motion', C.c_uint32), ('moved', C.c_bool)]


class Steps(C.Structure):
    _fields_ = [('steps', C.c_int32), ('reversed', C.c_bool)]


class FirmwareTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        so = Path(cls.temp.name) / 'motion.so'
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-shared', '-fPIC', str(SOURCE / 'point_motion.c'), str(SOURCE / 'scroll_policy.c'), '-o', str(so)], check=True)
        cls.lib = C.CDLL(str(so))
        cls.lib.sval_scroll_policy_update.argtypes = [C.POINTER(Policy), C.c_int32, C.c_int32, C.c_uint16]
        cls.lib.sval_scroll_policy_update.restype = C.c_bool
        cls.lib.sval_scroll_policy_axes.argtypes = [C.POINTER(Policy), C.c_bool]
        cls.lib.sval_scroll_policy_axes.restype = C.c_uint8
        cls.lib.sval_point_turn.argtypes = [C.POINTER(Turn), C.c_int32, C.c_int32, C.c_int32, C.c_uint32]
        cls.lib.sval_point_turn.restype = Steps
        cls.lib.sval_point_normalize.argtypes = [C.c_int16, C.c_uint16, C.POINTER(C.c_int64)]
        cls.lib.sval_point_normalize.restype = C.c_int32

    def compile_run(self, name, files):
        exe = Path(self.temp.name) / name
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=undefined', '-g', '-I' + str(HERE / 'stubs'), '-I' + str(SOURCE), '-I' + str(SOURCE.parent),
                        str(HERE / (name + '.c')), *[str(SOURCE / f) for f in files], '-o',
                        str(exe)],
                       check=True)
        subprocess.run([str(exe)], check=True)

    def test_output_broker(self):
        self.compile_run('test_output', ['point_output.c'])

    def test_controller(self):
        self.compile_run('test_control', ['point_output.c', 'point_motion.c', 'point_proportional.c', 'point_config.c', str(HERE / 'storage_stub.c')])

    def test_flicks(self):
        self.compile_run('test_flick', ['point_output.c', 'point_motion.c', 'point_proportional.c', 'point_config.c', str(HERE / 'storage_stub.c')])

    def test_proportional(self):
        self.compile_run('test_proportional', ['point_proportional.c'])

    def test_tools(self):
        self.compile_run('test_tools', ['point_output.c', 'point_motion.c', 'point_proportional.c', 'point_config.c', str(HERE / 'storage_stub.c')])

    def test_config(self):
        self.compile_run('test_config', ['point_config.c', str(HERE / 'storage_stub.c')])

    def test_storage_map(self):
        # VIA custom config ranges never overlap; the firmware also checks this at build time.
        self.compile_run('test_storage_map', [])
        config = (SOURCE.parent / 'config.h').read_text()
        self.assertIn('#define VIA_EEPROM_CUSTOM_CONFIG_SIZE SVAL_VIA_CUSTOM_SIZE', config)

    def test_keybard_vectors(self):
        # Payloads encoded by Keybard's encodePointControls(); the firmware must read them as meant.
        exe = Path(self.temp.name) / 'test_keybard_vectors'
        subprocess.run([
            'cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=undefined', '-g', '-I' + str(HERE / 'stubs'), '-I' + str(SOURCE), '-I' + str(SOURCE.parent),
            str(HERE / 'test_keybard_vectors.c'),
            str(SOURCE / 'point_config.c'),
            str(HERE / 'storage_stub.c'), '-o',
            str(exe)
        ],
                       check=True)
        subprocess.run([str(exe), str(HERE / 'keybard_vectors.txt')], check=True)

    def test_combined_adapter(self):
        original = (ROOT / 'keyboards/svalboard/keymaps/keymap_support.c').read_text()
        original = original[original.index('#define SCROLL_FREQUENCY_MS'):original.index('void toggle_axis_scroll_lock')]
        (Path(self.temp.name) / 'adapter.inc').write_text(original)
        exe = Path(self.temp.name) / 'adapter'
        subprocess.run([
            'cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=undefined', '-DSVAL_POINT_CONTROLS', '-DAXIS_TYPE=int16_t', '-I' + self.temp.name, '-I' + str(HERE / 'stubs'), '-I' + str(SOURCE), '-I' + str(SOURCE.parent),
            str(HERE / 'test_adapter.c'),
            str(SOURCE / 'point_motion.c'),
            str(SOURCE / 'point_proportional.c'),
            str(SOURCE / 'point_output.c'),
            str(SOURCE / 'point_config.c'),
            str(HERE / 'storage_stub.c'),
            str(SOURCE / 'scroll_policy.c'),
            str(SOURCE.parent / 'axis_scale.c'), '-o',
            str(exe)
        ],
                       check=True)
        subprocess.run([str(exe)], check=True)

    def test_legacy_policy_equivalence(self):
        # Compile the exact pre-extraction function, not a rewritten reference.
        original = subprocess.check_output(['git', 'show', '9e7bc919872fb0fd4517796de47021bdbc6b3d36:keyboards/svalboard/keymaps/keymap_support.c'], cwd=ROOT, text=True)
        original = original[original.index('#define AXIS_LOCK_BREAKAWAY_THRESHOLD'):original.index('void update_sniper_divisor')]
        fixture = Path(self.temp.name) / 'legacy.c'
        fixture.write_text(
            '''#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
static uint16_t now;
uint16_t timer_read(void) { return now; }
uint16_t timer_elapsed(uint16_t old) { return now-old; }
static int32_t scroll_accumulator_h, scroll_accumulator_v, m_scroll_accumulator_h, m_scroll_accumulator_v;
''' + original + '''
int sample(int32_t h, int32_t v, uint16_t t) { now=t; scroll_accumulator_h=scroll_accumulator_v=1; update_axis_scroll_mode(h,v); return scroll_accumulator_h == 0; }
int mode(void) { return axis_scroll_mode; }
int accum_h(void) { return axis_lock_accum_h; }
int accum_v(void) { return axis_lock_accum_v; }
'''
        )
        so = Path(self.temp.name) / 'legacy.so'
        subprocess.run(['cc', '-shared', '-fPIC', str(fixture), '-o', str(so)], check=True)
        old = C.CDLL(str(so))
        old.sample.argtypes = [C.c_int32, C.c_int32, C.c_uint16]
        policy = Policy()
        rng = random.Random(0x3389)
        now = 0
        samples = [(0, 0, 0), (1, 1, 1), (20000, 0, 2), (0, 0, 502), (0, 0, 503)]
        samples += [(rng.randint(-3000, 3000), rng.randint(-3000, 3000), rng.randint(1, 30)) for _ in range(10000)]
        for h, v, dt in samples:
            now = (now + dt) & 65535
            self.assertEqual(bool(old.sample(h, v, now)), self.lib.sval_scroll_policy_update(C.byref(policy), h, v, now))
            self.assertEqual((old.mode(), old.accum_h(), old.accum_v()), (policy.mode, policy.h, policy.v))
            self.assertEqual(self.lib.sval_scroll_policy_axes(C.byref(policy), False), 3)
            self.assertEqual(self.lib.sval_scroll_policy_axes(C.byref(policy), True), 2 if policy.mode == 1 else 1)

    def test_reversal_partition_and_idle(self):
        scale = 65536
        state = Turn()

        def feed(delta, t):
            return self.lib.sval_point_turn(C.byref(state), delta * scale, 100 * scale, 8 * scale, t).steps

        self.assertEqual(feed(80, 0), 0)
        self.assertEqual(feed(-100, 1), -1)
        self.assertEqual(feed(90, 2), 0)
        self.assertEqual(feed(10, 252), 0)
        for seed in range(100):
            rng = random.Random(seed)
            distance = rng.randint(1, 1000)
            a = Turn()
            b = Turn()
            expected = self.lib.sval_point_turn(C.byref(a), distance * scale, 64 * scale, 8 * scale, 1).steps
            total = 0
            remaining = distance
            while remaining:
                piece = rng.randint(1, remaining)
                remaining -= piece
                total += self.lib.sval_point_turn(C.byref(b), piece * scale, 64 * scale, 8 * scale, 1).steps
            self.assertEqual((expected, a.progress), (total, b.progress))

    def test_normalization_and_extremes(self):
        rem = C.c_int64()
        total = sum(self.lib.sval_point_normalize(1, 36000, C.byref(rem)) for _ in range(450))
        self.assertEqual(total, 10 * 65536)
        self.assertEqual(self.lib.sval_point_normalize(-32768, 1, C.byref(rem)), -2147483647)
        self.assertEqual(self.lib.sval_point_normalize(100, 0, C.byref(rem)), 0)


if __name__ == '__main__':
    unittest.main()
