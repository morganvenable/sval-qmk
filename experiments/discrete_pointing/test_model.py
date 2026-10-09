"""Behavioral contracts for the design model; no production firmware imported."""
import random
import unittest
from fractions import Fraction

from model import Activations, Binding, FreshIntent, SignedBucket, TurnRecognizer, normalized


class MotionContracts(unittest.TestCase):
    def test_first_step_needs_full_distance_and_exact_boundary_is_enough(self):
        m = TurnRecognizer()
        self.assertEqual(m.feed(99, 0).steps, 0)
        self.assertEqual(m.feed(1, 1).steps, 1)

    def test_large_packet_conserves_remainder(self):
        m = TurnRecognizer()
        self.assertEqual(m.feed(260, 0).steps, 2)
        self.assertEqual(m.feed(40, 1).steps, 1)
        self.assertEqual(m.progress, 0)

    def test_reversal_does_not_require_repaying_old_positive_progress(self):
        m, baseline = TurnRecognizer(), SignedBucket()
        m.feed(80, 0)
        baseline.feed(80)
        result = m.feed(-100, 10)
        self.assertTrue(result.reversed)
        self.assertEqual(result.steps, -1)
        self.assertEqual(baseline.feed(-100), 0)  # distinguishes the policies

    def test_reversal_distance_is_credited_not_added_as_extra_deadzone(self):
        m = TurnRecognizer()
        m.feed(80, 0)
        self.assertTrue(m.feed(-15, 1).reversed)
        self.assertEqual(m.feed(-85, 2).steps, -1)

    def test_small_reverse_excursion_must_be_repaid_before_forward_progress(self):
        m = TurnRecognizer()
        m.feed(95, 0)
        self.assertFalse(m.feed(-10, 1).reversed)
        self.assertEqual(m.feed(10, 2).steps, 0)
        self.assertEqual(m.feed(5, 3).steps, 1)

    def test_oscillation_cannot_manufacture_distance(self):
        m = TurnRecognizer()
        m.feed(60, 0)
        outputs = [m.feed(-5 if i % 2 else 5, i).steps for i in range(1, 1001)]
        self.assertEqual(sum(abs(x) for x in outputs), 0)
        self.assertEqual(m.progress, 60)

    def test_qualified_turns_deliberately_abandon_unfinished_strokes(self):
        m = TurnRecognizer()
        self.assertEqual(m.feed(90, 0).steps, 0)
        self.assertEqual(m.feed(-90, 1).steps, 0)
        self.assertEqual(m.feed(90, 2).steps, 0)

    def test_idle_boundary_resets_partial_step(self):
        m = TurnRecognizer()
        m.feed(60, 0)
        self.assertEqual(m.feed(40, 250).steps, 0)
        self.assertEqual(m.feed(60, 251).steps, 1)

    def test_no_motion_does_not_keep_stale_progress_alive(self):
        m = TurnRecognizer()
        m.feed(60, 0)
        for t in [50, 100, 200, 249]:
            m.feed(0, t)
        self.assertEqual(m.feed(40, 250).steps, 0)

    def test_independent_pointers_do_not_share_fractional_motion(self):
        left, right = TurnRecognizer(), TurnRecognizer()
        left.feed(60, 0)
        self.assertEqual(right.feed(40, 1).steps, 0)
        self.assertEqual(left.feed(40, 1).steps, 1)

    def test_same_distance_at_different_effective_cpi(self):
        for cpi in [200, 400, 800, 1600, 3200, 12000]:
            m = TurnRecognizer(distance=64, reversal=8)
            self.assertEqual(m.feed(normalized(cpi * 2 // 25, cpi), 0).steps, 1)

    def test_sub_reference_unit_motion_is_not_lost_per_report(self):
        m = TurnRecognizer(distance=64, reversal=8)
        outputs = [m.feed(normalized(1, 12000), i).steps for i in range(960)]
        self.assertEqual(sum(outputs), 1)
        self.assertEqual(m.progress, 0)

    def test_sniper_gain_requires_more_travel(self):
        m = TurnRecognizer(distance=64, reversal=8)
        self.assertEqual(m.feed(normalized(64, 800) * Fraction(1, 2), 0).steps, 0)
        self.assertEqual(m.feed(normalized(64, 800) * Fraction(1, 2), 1).steps, 1)

    def test_reset_never_produces_output(self):
        m = TurnRecognizer()
        m.feed(99, 0)
        m.reset()
        self.assertEqual(m.feed(1, 1).steps, 0)

    def test_sign_symmetry(self):
        a, b = TurnRecognizer(), TurnRecognizer()
        for t, dx in enumerate([91, -7, 4, 25, -45, -66, 200, -30]):
            self.assertEqual(a.feed(dx, t).steps, -b.feed(-dx, t).steps)

    def test_report_partitioning_does_not_change_observed_path_results(self):
        rng = random.Random(4817)
        whole, partitioned = TurnRecognizer(), TurnRecognizer()
        for t in range(150):
            dx = rng.randint(-220, 220)
            expected = whole.feed(dx, t)
            steps = 0
            for _ in range(abs(dx)):
                steps += partitioned.feed(1 if dx > 0 else -1, t).steps
            self.assertEqual(expected.steps, steps)
            self.assertEqual(whole.progress, partitioned.progress)
            self.assertEqual(whole.reverse_travel, partitioned.reverse_travel)
            self.assertEqual(whole.direction, partitioned.direction)

    def test_invalid_parameters_are_rejected(self):
        for distance, reversal in [(0, 1), (100, 0), (10, 11)]:
            with self.assertRaises(ValueError):
                TurnRecognizer(distance, reversal)
        with self.assertRaises(ValueError):
            normalized(10, 0)


class TemporalContracts(unittest.TestCase):
    def test_fast_roll_has_at_most_one_pending_step(self):
        q = FreshIntent()
        q.offer(500, 0)
        self.assertEqual(q.take(0), 1)
        self.assertEqual(q.take(80), 0)
        self.assertEqual(q.coalesced, 499)

    def test_rate_limit_and_one_fresh_trailing_step(self):
        q = FreshIntent()
        q.offer(1, 0)
        self.assertEqual(q.take(0), 1)
        q.offer(10, 10)
        self.assertEqual(q.take(79), 0)
        self.assertEqual(q.take(80), 1)
        self.assertEqual(q.take(160), 0)

    def test_busy_output_expires_instead_of_replaying_stale_motion(self):
        q = FreshIntent()
        q.offer(1, 0)
        self.assertEqual(q.take(40, resource_ready=False), 0)
        self.assertEqual(q.take(80, resource_ready=True), 0)
        self.assertEqual(q.expired, 1)

    def test_qualified_reversal_clears_forward_intent_before_reverse_step(self):
        m, q = TurnRecognizer(), FreshIntent()
        q.offer(m.feed(100, 0).steps, 0)
        turn = m.feed(-15, 1)
        self.assertTrue(turn.reversed)
        self.assertEqual(turn.steps, 0)
        if turn.reversed:
            q.clear()
        self.assertEqual(q.take(2), 0)

    def test_generation_boundary_invalidates_pending_and_stale_producers(self):
        q = FreshIntent()
        generation = q.generation
        q.offer(1, 0)
        q.new_generation()
        q.offer(1, 1, generation=generation)
        self.assertEqual(q.take(2), 0)

    def test_generation_change_does_not_bypass_rate_limit(self):
        q = FreshIntent()
        q.offer(1, 0)
        q.take(0)
        q.new_generation()
        q.offer(1, 1)
        self.assertEqual(q.take(1), 0)
        self.assertEqual(q.take(80), 1)


class ActivationContracts(unittest.TestCase):
    def test_nested_holds_out_of_order_release_and_toggle_fallback(self):
        a = Activations()
        a.toggle('volume', Binding('right', 'volume'))
        a.hold('key1', Binding('right', 'tabs'))
        a.hold('key2', Binding('right', 'arrows'))
        a.release('key1')
        self.assertEqual(a.winners['right'].control, 'arrows')
        a.release('key2')
        self.assertEqual(a.winners['right'].control, 'volume')

    def test_two_keys_for_one_control_are_distinct_owners(self):
        a = Activations()
        a.hold('key1', Binding('left', 'volume'))
        a.hold('key2', Binding('left', 'volume'))
        generation = a.generations['left']
        a.release('key2')
        self.assertEqual(a.winners['left'].token, 'key1')
        self.assertGreater(a.generations['left'], generation)

    def test_toggle_replaces_instead_of_creating_hidden_toggle_stack(self):
        a = Activations()
        a.toggle('volume', Binding('left', 'volume'))
        a.toggle('tabs', Binding('left', 'tabs'))
        a.toggle('tabs', Binding('left', 'tabs'))
        self.assertIsNone(a.winners['left'])

    def test_pointer_ownership_is_independent(self):
        a = Activations()
        a.hold('key1', Binding('left', 'volume'))
        a.hold('key2', Binding('right', 'tabs'))
        a.release('key1')
        self.assertIsNone(a.winners['left'])
        self.assertEqual(a.winners['right'].control, 'tabs')

    def test_completed_hold_can_be_inhibited_until_release(self):
        a = Activations()
        a.hold('key1', Binding('right', 'tasks'))
        a.inhibit_winner('right')
        self.assertIsNone(a.winners['right'])
        self.assertFalse(a.hold('key1', Binding('right', 'tasks')))
        a.release('key1')
        self.assertTrue(a.hold('key1', Binding('right', 'tasks')))

    def test_capacity_rejects_new_owner_without_disturbing_previous_one(self):
        a = Activations(capacity=1)
        a.hold('key1', Binding('right', 'volume'))
        self.assertFalse(a.hold('key2', Binding('right', 'tabs')))
        self.assertEqual(a.winners['right'].control, 'volume')

    def test_releasing_an_unknown_token_is_harmless(self):
        a = Activations()
        a.hold('key1', Binding('right', 'volume'))
        a.release('unrelated-key')
        self.assertEqual(a.winners['right'].control, 'volume')


if __name__ == '__main__':
    unittest.main()
