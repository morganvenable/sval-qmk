import ctypes
import unittest
from unittest.mock import patch
import windows_touchpad as w


class Call:
    def __init__(self, fn): self.fn = fn
    def __call__(self, *args): return self.fn(*args)


class BackendTests(unittest.TestCase):
    def setUp(self):
        self.frames, self.destroyed, self.created = [], [], []
        def create(params):
            self.created.append(params)
            return len(self.created)
        def inject(handle, reports, count):
            self.frames.append([(r.touchInfo.pointerInfo.pointerId,
                                 r.touchInfo.pointerInfo.pointerFlags,
                                 r.touchInfo.pointerInfo.ptHimetricLocation.x) for r in reports])
            return 1
        class DLL: pass
        self.dll = DLL()
        self.dll.CreateSyntheticPointerDevice2 = Call(create)
        self.dll.InjectSyntheticPointerInput = Call(inject)
        self.dll.DestroySyntheticPointerDevice = Call(self.destroyed.append)
        self.mock = patch.object(w, 'user32', return_value=self.dll)
        self.mock.start()
        self.addCleanup(self.mock.stop)

    def test_replacement_never_exceeds_five_reports(self):
        with w.WindowsTouchpad() as pad:
            pad.frame({i: (100 * i, 300) for i in range(1, 6)})
            pad.frame({i: (100 * i, 300) for i in range(6, 11)})
        self.assertEqual([len(f) for f in self.frames], [5, 5, 5])
        self.assertTrue(all(flags == w.CONFIDENCE for _, flags, _ in self.frames[1]))
        self.assertEqual(self.destroyed, [1])

    def test_physical_lift_keeps_last_coordinates(self):
        with w.WindowsTouchpad() as pad:
            pad.frame({3: (1200, 300)})
            pad.frame({})
        self.assertEqual(self.frames[-1], [(3, w.CONFIDENCE, 1200)])

    def test_failure_cancel_recreates_without_normal_lift(self):
        with w.WindowsTouchpad() as pad:
            pad.frame({1: (100, 200)})
            pad.cancel()
            self.assertEqual(pad.previous, {})
            self.assertEqual(len(self.frames), 1)
            self.assertEqual(self.destroyed, [1])
            self.assertEqual(len(self.created), 2)
            pad.cancel()
            self.assertEqual(len(self.created), 2)
        self.assertEqual(self.destroyed, [1, 2])

    def test_invalid_input_never_reaches_windows(self):
        with w.WindowsTouchpad() as pad:
            with self.assertRaises(ValueError): pad.frame({1: (4301, 0)})
            with self.assertRaises(ValueError): pad.frame({i: (0, 0) for i in range(6)})
        self.assertEqual(self.frames, [])
