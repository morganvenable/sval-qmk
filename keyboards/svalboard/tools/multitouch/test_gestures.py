import math
import unittest
from gestures import BallVectors, Sample, ThreeFingerSwipe, TwoHand


class TwoHandTests(unittest.TestCase):
    def begin(self):
        g = TwoHand()
        g.set_clutch(True)
        for hand in (0, 1): g.feed(hand, Sample(0, 65535, 1, {7: (20, 20)}))
        self.assertEqual(g.snapshot(0).scale, 1)
        return g

    def test_pan_zoom_rotation_and_sequence_wrap(self):
        for points, pan, scale, angle in [
            ([(24, 23), (24, 23)], (4, 3), 1, 0),
            ([(15, 20), (25, 20)], (0, 0), 1.5, 0),
            ([(30, 10), (10, 30)], (0, 0), 1, 90),
        ]:
            g = self.begin()
            for h in (0, 1): g.feed(h, Sample(.01, 0, 1, {7: points[h]}))
            result = g.snapshot(.01)
            self.assertEqual(result.pan, pan)
            self.assertAlmostEqual(result.scale, scale)
            self.assertAlmostEqual(result.rotation, angle)

    def test_brief_contact_loss_requires_reclutch(self):
        g = self.begin()
        g.feed(0, Sample(.01, 0, 1, {}))
        g.feed(0, Sample(.02, 1, 1, {7: (20, 20)}))
        self.assertIsNone(g.snapshot(.02))
        self.assertTrue(g.blocked)
        g.set_clutch(False); g.set_clutch(True)
        self.assertIsNotNone(g.snapshot(.02))

    def test_faults_cancel(self):
        for s in [Sample(.01, 1, 1, {7: (20, 20)}),
                  Sample(.01, 0, 2, {7: (20, 20)}),
                  Sample(.01, 0, 1, {8: (20, 20)}),
                  Sample(.01, 0, 1, {7: (20, 20), 8: (25, 20)}),
                  Sample(.01, 0, 1, {7: (math.nan, 20)}),
                  Sample(.01, 0, 1, {7: (20, 20)}, False)]:
            g = self.begin(); g.feed(0, s)
            self.assertIsNone(g.snapshot(.01)); self.assertTrue(g.blocked)

    def test_duplicates_do_not_keep_stationary_hand_alive(self):
        g = self.begin()
        g.feed(0, Sample(.04, 65535, 1, {7: (20, 20)}))
        self.assertIsNone(g.snapshot(.051)); self.assertTrue(g.blocked)

    def test_skew_waits_then_stale_cancels(self):
        g = self.begin(); g.feed(0, Sample(.03, 0, 1, {7: (21, 20)}))
        self.assertIsNone(g.snapshot(.03)); self.assertFalse(g.blocked)
        self.assertIsNone(g.snapshot(.051)); self.assertTrue(g.blocked)

    def test_unclutched_has_no_output(self):
        g = self.begin(); g.set_clutch(False)
        self.assertIsNone(g.snapshot(.01))


class SwipeTests(unittest.TestCase):
    def sample(self, seq, x=0, count=3):
        return Sample(seq*.01, seq, 1, {i: (x+i*5, 20) for i in range(count)})

    def test_once_until_all_up(self):
        g = ThreeFingerSwipe()
        self.assertIsNone(g.feed(self.sample(0)))
        self.assertEqual(g.feed(self.sample(1, 9)), 'right')
        self.assertIsNone(g.feed(self.sample(2, 18)))
        g.feed(self.sample(3, count=0)); g.feed(self.sample(4))
        self.assertEqual(g.feed(self.sample(5, -9)), 'left')

    def test_dropout_does_not_resume_swipe(self):
        g = ThreeFingerSwipe(); g.feed(self.sample(0))
        g.feed(self.sample(1, 2, count=2))
        self.assertIsNone(g.feed(self.sample(2, 12)))

    def test_two_plus_one_is_not_three_finger_swipe(self):
        g = ThreeFingerSwipe(); g.feed(self.sample(0))
        self.assertIsNone(g.feed(self.sample(1, 9), other_hand_active=True))
        self.assertIsNone(g.feed(self.sample(2, 18)))

    def test_incoherent_motion_and_gaps(self):
        g = ThreeFingerSwipe(); g.feed(self.sample(0))
        self.assertIsNone(g.feed(Sample(.01, 1, 1, {0: (30,20), 1: (5,20), 2: (10,20)})))
        self.assertIsNone(g.feed(self.sample(3, 20)))


class BallTests(unittest.TestCase):
    def test_relative_motion_and_fresh_reanchor(self):
        b = BallVectors(1); b.set_clutch(True)
        for h in (0,1): b.feed(h, 0, 0, 0)
        b.engine.snapshot(0)
        b.feed(0, -5, 0, .01); b.feed(1, 5, 0, .01)
        self.assertEqual(b.engine.snapshot(.01).scale, 1.5)
        b.set_clutch(False); b.set_clutch(True)
        b.feed(0, 0, 0, .02)
        self.assertIsNone(b.engine.snapshot(.02))
        b.feed(1, 0, 0, .02)
        self.assertEqual(b.engine.snapshot(.02).scale, 1)
        self.assertIsNone(b.engine.snapshot(.08))


if __name__ == '__main__': unittest.main()
