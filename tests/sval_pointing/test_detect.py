"""Run pointing_detect.c's probes against simulated modules on the host; no device is accessed."""
from pathlib import Path
import json
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent


class PointingDetectTests(unittest.TestCase):
    def test_probes_find_each_module_and_leave_the_pins_released(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / 'detect'
            subprocess.run(['gcc', '-std=gnu11', '-Wall', '-Werror', '-DSVAL_DETECT_HOST_TEST', '-I',
                            str(HERE), '-I',
                            str(ROOT / 'keyboards/svalboard'),
                            str(ROOT / 'keyboards/svalboard/pointing_detect.c'),
                            str(HERE / 'sim.c'), '-o',
                            str(binary)],
                           check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('OK', result.stdout)


class PointerReprobeTests(unittest.TestCase):
    """pointer_reprobe.c: the runtime search's schedule, stopping after a find, never probing a working half."""
    def test_search_schedule_and_stopping(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / 'reprobe'
            subprocess.run([
                'gcc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-DSVAL_REPROBE_HOST_TEST', '-I',
                str(HERE), '-I',
                str(ROOT / 'keyboards/svalboard'),
                str(ROOT / 'keyboards/svalboard/pointer_reprobe.c'),
                str(HERE / 'reprobe_test.c'), '-o',
                str(binary)
            ],
                           check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('OK', result.stdout)


class PointingMotionTests(unittest.TestCase):
    """pointing_motion.h: fragment ids for each detected module (0x18) and the 0x2D packing."""
    def test_fragment_mapping_and_motion_packing(self):
        with tempfile.TemporaryDirectory() as directory:
            header = Path(directory) / 'sval_config.h'
            subprocess.run([sys.executable, '-I', str(ROOT / 'modules/svalboard/core/sval_config.py'), str(ROOT / 'keyboards/svalboard/keymaps/sval/sval.json'), str(header)], check=True, capture_output=True)
            binary = Path(directory) / 'motion'
            subprocess.run(['gcc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-I', directory, '-I', str(ROOT / 'keyboards/svalboard'), str(HERE / 'motion_test.c'), '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('OK', result.stdout)

    def test_both_keymaps_define_the_pointing_instances(self):
        for keymap in ('sval', 'blank'):
            data = json.loads((ROOT / f'keyboards/svalboard/keymaps/{keymap}/sval.json').read_text())
            ids = [inst['id'] for inst in data['composition']['instances']]
            self.assertEqual(ids[10:], ['left_pointing', 'right_pointing'], keymap)
            for inst in data['composition']['instances'][10:]:
                self.assertTrue(inst['hardware_detect'])
                for opt in inst['fragment_options']:
                    frag = data['fragments'][opt['fragment']]
                    self.assertEqual(frag['kle'], [])
                    self.assertEqual(frag['widget']['kind'], 'pointing')
                    self.assertEqual(opt['matrix_map'], [])


if __name__ == '__main__':
    unittest.main()
