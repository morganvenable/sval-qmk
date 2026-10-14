"""Two-hand manipulation and single-hand swipe prototypes; no OS side effects.

Coordinates are calibrated millimetres. A clutch explicitly authorizes two-hand
manipulation. Faults require clutch release/re-press; clocks are host monotonic.
"""
from dataclasses import dataclass
import math


@dataclass(frozen=True)
class Sample:
    time: float
    sequence: int
    epoch: int
    contacts: dict
    valid: bool = True


@dataclass(frozen=True)
class Manipulation:
    contacts: dict
    pan: tuple
    scale: float
    rotation: float


class TwoHand:
    def __init__(self, stale=.05, max_skew=.025):
        self.stale, self.max_skew = stale, max_skew
        self.samples = {}
        self.clutch = False
        self.blocked = False
        self.origin = None
        self.identities = None
        self.last_output = None
        self.reason = 'Hold clutch to manipulate'

    def set_clutch(self, held):
        held = bool(held)
        if held != self.clutch:
            self.origin = self.identities = self.last_output = None
            self.blocked = False
            self.reason = 'Waiting for one fresh contact per hand' if held else 'Clutch released'
        self.clutch = held

    def cancel(self, reason):
        self.origin = self.identities = self.last_output = None
        self.blocked = self.clutch
        self.reason = reason + '; release clutch to rearm'

    def feed(self, hand, sample):
        if hand not in (0,1): raise ValueError('Hand must be 0 or 1')
        previous = self.samples.get(hand)
        if previous:
            delta = (sample.sequence - previous.sequence) & 65535
            if delta == 0 and sample.epoch == previous.epoch:
                return  # duplicate packets do not extend freshness
            if sample.time < previous.time:
                self.cancel('Out-of-order sample'); return
            if delta != 1 or sample.epoch != previous.epoch:
                self.cancel('Contact stream discontinuity')
        self.samples[hand] = sample
        if not sample.valid:
            self.cancel('Invalid contact sample')
        elif self.origin is not None and (len(sample.contacts) != 1 or
                next(iter(sample.contacts)) != self.identities[hand]):
            # A brief lift between render ticks still ends the manipulation.
            self.cancel('Contact lifted or identity changed')

    def snapshot(self, now):
        if not self.clutch or self.blocked: return None
        if len(self.samples) != 2: return None
        left, right = self.samples[0], self.samples[1]
        if any(not s.valid or now-s.time>self.stale or now<s.time for s in (left,right)):
            self.cancel('Stale hand'); return None
        if any(len(s.contacts)!=1 for s in (left,right)):
            if self.origin is not None: self.cancel('Contact lifted or extra finger')
            return None
        if abs(left.time-right.time)>self.max_skew:
            self.reason='Waiting for temporally aligned samples'; return None
        identities = (next(iter(left.contacts)), next(iter(right.contacts)))
        points = (left.contacts[identities[0]], right.contacts[identities[1]])
        if any(not math.isfinite(v) for p in points for v in p):
            self.cancel('Invalid coordinates'); return None
        if self.origin is None:
            self.origin, self.identities = points, identities
        elif identities != self.identities:
            self.cancel('Contact identity changed'); return None
        virtual = {i+1: (anchor[0]+points[i][0]-self.origin[i][0], anchor[1]+points[i][1]-self.origin[i][1])
                   for i,anchor in enumerate(((40.,50.),(60.,50.)))}
        a,b = virtual[1],virtual[2]
        dx,dy=b[0]-a[0],b[1]-a[1]
        distance=math.hypot(dx,dy)
        if distance<2 or any(not 0<=v<=100 for p in virtual.values() for v in p):
            self.cancel('Virtual contact limit'); return None
        self.reason='Two-hand manipulation active'
        self.last_output=Manipulation(virtual,((a[0]+b[0])/2-50,(a[1]+b[1])/2-50),distance/20,math.degrees(math.atan2(dy,dx)))
        return self.last_output


class ThreeFingerSwipe:
    """Recognize one horizontal swipe per all-up cycle, on one hand only."""
    def __init__(self, threshold_mm=8, deadline=.8):
        self.threshold, self.deadline = threshold_mm, deadline
        self.start = None
        self.locked = False
        self.previous = None

    def feed(self, sample, other_hand_active=False):
        if self.previous is not None and sample.epoch == self.previous.epoch and sample.sequence == self.previous.sequence:
            return None
        discontinuity = self.previous is not None and (sample.epoch != self.previous.epoch or
            ((sample.sequence-self.previous.sequence)&65535)!=1 or sample.time-self.previous.time>.05 or sample.time<self.previous.time)
        self.previous = sample
        if not sample.valid or discontinuity or other_hand_active or any(
                not math.isfinite(v) for p in sample.contacts.values() for v in p):
            self.start=None; self.locked=True; return None
        if not sample.contacts:
            self.start=None; self.locked=False; return None
        if self.locked: return None
        if len(sample.contacts)!=3:
            if self.start is not None:self.locked=True
            self.start=None; return None
        if self.start is None:self.start=sample;return None
        if sample.contacts.keys()!=self.start.contacts.keys() or sample.time-self.start.time>self.deadline:
            self.start=None;self.locked=True;return None
        vectors=[(p[0]-self.start.contacts[k][0],p[1]-self.start.contacts[k][1]) for k,p in sample.contacts.items()]
        dx=sum(p[0] for p in vectors)/3;dy=sum(p[1] for p in vectors)/3
        if abs(dx)>=self.threshold and abs(dx)>1.5*abs(dy) and all(v[0]*dx>0 and abs(v[0])>=self.threshold/2 for v in vectors):
            self.locked=True
            return 'right' if dx>0 else 'left'
        return None


class BallVectors:
    """Relative-delta adapter for the same two-hand engine, active only in a clutch."""
    def __init__(self, mm_per_count=.05):
        self.engine=TwoHand();self.gain=mm_per_count
        self.positions=[[0.,0.],[0.,0.]];self.sequences=[0,0]

    def set_clutch(self, held):
        if bool(held)!=self.engine.clutch:
            self.positions=[[0.,0.],[0.,0.]]
            self.engine.samples.clear()
        self.engine.set_clutch(held)

    def feed(self, hand, dx, dy, now):
        if not self.engine.clutch:return
        self.positions[hand][0]+=dx*self.gain;self.positions[hand][1]+=dy*self.gain
        self.sequences[hand]=(self.sequences[hand]+1)&65535
        self.engine.feed(hand,Sample(now,self.sequences[hand],0,{1:tuple(self.positions[hand])}))
        # The caller must supply live zero-motion heartbeats for a stationary ball.
