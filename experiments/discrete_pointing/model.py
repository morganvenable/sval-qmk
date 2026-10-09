"""Executable behavioral specification; deliberately NOT a firmware implementation.

Exact rational distances isolate gesture semantics from fixed-point rounding.
No USB, QMK, persistent storage, axis selection, or OS behavior is modeled.
"""
from dataclasses import dataclass
from fractions import Fraction


def normalized(delta: int, effective_cpi: int) -> Fraction:
    if effective_cpi <= 0:
        raise ValueError('effective CPI must be positive')
    return Fraction(delta * 800, effective_cpi)


@dataclass(frozen=True)
class MotionResult:
    steps: int = 0
    reversed: bool = False


class TurnRecognizer:
    """One-dimensional distance recognition with qualified turns.

    A turn credits the complete opposite excursion, but abandons unfinished
    travel in the former direction. Times are unbounded monotonic model times;
    the firmware implementation must separately establish wrap-safe arithmetic.
    """
    def __init__(self, distance=100, reversal=15, idle_ms=250):
        self.distance = Fraction(distance)
        self.reversal = Fraction(reversal)
        if not 0 < self.reversal <= self.distance:
            raise ValueError('require 0 < reversal <= distance')
        if idle_ms <= 0:
            raise ValueError('idle interval must be positive')
        self.idle_ms = idle_ms
        self.last_seen = None
        self.reset()

    def reset(self):
        self.direction = 0
        self.progress = Fraction(0)
        self.reverse_travel = Fraction(0)
        self.last_motion = None

    def feed(self, delta, now_ms) -> MotionResult:
        if self.last_seen is not None and now_ms < self.last_seen:
            raise ValueError('time moved backwards')
        self.last_seen = now_ms
        if self.last_motion is not None and now_ms - self.last_motion >= self.idle_ms:
            self.reset()
        delta = Fraction(delta)
        if not delta:
            return MotionResult()
        self.last_motion = now_ms
        if not self.direction:
            self.direction = 1 if delta > 0 else -1
        forward = delta * self.direction
        reversed_direction = False
        if forward >= 0:
            repayment = min(forward, self.reverse_travel)
            self.reverse_travel -= repayment
            self.progress += forward - repayment
        else:
            self.reverse_travel -= forward
            if self.reverse_travel >= self.reversal:
                self.direction = -self.direction
                self.progress = self.reverse_travel
                self.reverse_travel = Fraction(0)
                reversed_direction = True
        count = int(self.progress // self.distance)
        self.progress -= count * self.distance
        return MotionResult(count * self.direction, reversed_direction)


class SignedBucket:
    """Comparison policy: signed remainder, with no qualified-turn rule."""
    def __init__(self, distance=100):
        self.distance = Fraction(distance)
        self.remainder = Fraction(0)

    def feed(self, delta):
        self.remainder += delta
        count = int(self.remainder / self.distance)  # truncation toward zero
        self.remainder -= count * self.distance
        return count


@dataclass(frozen=True)
class Pending:
    direction: int
    offered_ms: int
    generation: int


class FreshIntent:
    """One-slot admission limiter. 'Taken' is NOT host/USB acknowledgement.

    In firmware, take is committed only when an output resource can accept a
    new pulse. The output broker must continue owning that pulse until release.
    """
    def __init__(self, interval_ms=80, max_age_ms=80):
        if interval_ms <= 0 or max_age_ms <= 0:
            raise ValueError('interval and age must be positive')
        self.interval_ms = interval_ms
        self.max_age_ms = max_age_ms
        self.generation = 0
        self.pending = None
        self.last_start = None
        self.offered = self.admitted = self.coalesced = self.expired = self.cancelled = 0

    def clear(self):
        if self.pending is not None:
            self.cancelled += 1
        self.pending = None

    def new_generation(self):
        self.clear()
        self.generation += 1
        # Do not let rapid mode changes bypass the output-rate limit.

    def expire(self, now_ms):
        if self.pending is not None and now_ms - self.pending.offered_ms >= self.max_age_ms:
            self.pending = None
            self.expired += 1

    def offer(self, steps, now_ms, *, generation=None):
        self.expire(now_ms)
        if not steps or (generation is not None and generation != self.generation):
            return
        direction = 1 if steps > 0 else -1
        self.offered += abs(steps)
        self.coalesced += abs(steps) - 1
        if self.pending is not None:
            self.coalesced += 1
        self.pending = Pending(direction, now_ms, self.generation)

    def take(self, now_ms, *, resource_ready=True):
        self.expire(now_ms)
        if self.pending is None or not resource_ready:
            return 0
        if self.last_start is not None and now_ms - self.last_start < self.interval_ms:
            return 0
        result = self.pending.direction
        self.pending = None
        self.last_start = now_ms
        self.admitted += 1
        return result


@dataclass(frozen=True)
class Binding:
    pointer: str
    control: str


@dataclass(frozen=True)
class Activation:
    kind: str
    token: str
    control: str


class Activations:
    """Hold/toggle precedence model, not QMK key-position/release integration.

    A generation changes when the winner's identity changes, even for the same
    control name. Runtime consumers must use that boundary to reset motion.
    Exclusive session teardown is specified in the design, not implemented here.
    """
    def __init__(self, capacity=8):
        self.capacity = capacity
        self.holds = {}  # insertion order is press order
        self.toggles = {'left': None, 'right': None}
        self.inhibited = set()
        self.winners = {'left': None, 'right': None}
        self.generations = {'left': 0, 'right': 0}

    def _resolve(self):
        for pointer in self.winners:
            winner = None
            for token, binding in reversed(list(self.holds.items())):
                if binding.pointer == pointer and token not in self.inhibited:
                    winner = Activation('hold', token, binding.control)
                    break
            if winner is None and self.toggles[pointer] is not None:
                token, control = self.toggles[pointer]
                winner = Activation('toggle', token, control)
            if winner != self.winners[pointer]:
                self.winners[pointer] = winner
                self.generations[pointer] += 1

    def hold(self, token, binding):
        if binding.pointer not in self.winners:
            raise ValueError('pointer must be left or right')
        if token in self.holds or len(self.holds) >= self.capacity:
            return False
        self.holds[token] = binding
        self._resolve()
        return True

    def release(self, token):
        self.holds.pop(token, None)
        self.inhibited.discard(token)
        self._resolve()

    def toggle(self, binding_id, binding):
        if binding.pointer not in self.winners:
            raise ValueError('pointer must be left or right')
        entry = (binding_id, binding.control)
        self.toggles[binding.pointer] = None if self.toggles[binding.pointer] == entry else entry
        self._resolve()

    def inhibit_winner(self, pointer):
        winner = self.winners[pointer]
        if winner is None:
            return
        if winner.kind == 'hold':
            self.inhibited.add(winner.token)
        else:
            self.toggles[pointer] = None
        self._resolve()


if __name__ == '__main__':
    # Reproducible, intentionally small behavioral examples.
    examples = {
        'qualified reversal': [(0, 80), (10, -100)],
        'small reverse wobble': [(0, 60), (10, -5), (20, 5), (30, 40)],
        'excess distance then reversal': [(0, 260), (10, -100)],
        'separate gestures': [(0, 60), (300, 40)],
    }
    print('case,time_ms,delta,signed_bucket_steps,turn_steps,turn_remainder')
    for name, trace in examples.items():
        turn, bucket = TurnRecognizer(), SignedBucket()
        for now, delta in trace:
            result = turn.feed(delta, now)
            print(f'{name},{now},{delta},{bucket.feed(delta)},{result.steps},{turn.progress}')
