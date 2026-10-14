"""Two independently clocked pads mapped onto one physical-size surface.

Never equate source clocks. Freshness uses host receipt time; a bounded latest
sample join tolerates a stationary hand, not a missing hand. No OS dependencies.
"""
import math
from collections import Counter


MERGE_MS=150  # cross-pad contacts join one Windows gesture only if they land this close together
ABSENT_S=.2   # a pad with only invalid reads, resets or stale snapshots this long is unplugged


class UnifiedPad:
    """One Windows touchpad from both pads.

    Ownership rules from the firmware's touch gesture settings (configure()):
    a frame flagged 0x40 means the firmware claimed the gesture, so Windows
    loses the contacts (destroy and recreate, no fake tap) until all-up; with
    the swipe owned by the firmware, touches of swipe_fingers or more never
    reach Windows; and a contact landing on the other pad more than MERGE_MS
    after the touch began is held back, unless the always-merge option is set.

    A pad that stops sending (split cable out) or sends only invalid reads,
    resets or stale snapshots for ABSENT_S (pad unplugged) drops out: the
    surface carries on with the other pad, after a cancel and a fresh all-up.
    """
    def __init__(self, backend=None, gap_mm=0, flips=((False,False),(False,False)), clock=None, merge_ms=MERGE_MS):
        import time
        if not math.isfinite(gap_mm) or not 0<=gap_mm<=50: raise ValueError('Gap must be 0–50 mm')
        self.backend, self.gap, self.flips = backend, round(gap_mm*100), flips
        self.clock=clock or time.monotonic
        self.width,self.height=8600+self.gap,4000
        self.samples={};self.ready=False;self.points={};self.reason='Lift all fingers on both pads to arm'
        self.cancellations=0;self.faults=Counter();self.gaps=0;self.identities={};self.next_id=1
        self.origin=None;self.metrics=None;self.last_output=None;self.last_tick=None
        self.merge_window=merge_ms/1000;self.always_merge=False;self.withhold_fingers=None;self.span=True
        self.session_pads=set();self.session_start=None;self.held=set()
        self.claims=0;self.withheld=0;self.owners='every gesture: Windows'
        self.bad_since={};self.present=None

    def configure(self,config):
        """config: gesture_config.GestureConfig from the firmware, or None (old firmware: Windows owns all)."""
        self.always_merge=bool(config and config.host_always_merge)
        self.span=bool(config is None or config.span_pads)
        self.withhold_fingers=config.swipe_fingers if config and config.firmware_swipe else None
        self.owners=config.summary() if config else 'every gesture: Windows (firmware has no gesture settings)'

    def cancel(self,reason,fault=True):
        if fault:self.faults[reason]+=1
        if self.points:
            self.cancellations+=1
            if self.backend:self.backend.cancel()
        self.ready=False;self.points={};self.identities={};self.next_id=1
        self.origin=self.metrics=self.last_output=None
        self.reason=reason+' · lift all fingers to rearm'

    def feed(self,frame,received=None):
        now=self.clock() if received is None else received
        previous=self.samples.get(frame.pad)
        reason=None
        if previous:
            old,when=previous
            delta=(frame.sequence-old.sequence)&65535
            if delta==0 and frame.epoch==old.epoch:return
            if now<when or now-when>.06:
                reason='Pad heartbeat interrupted'
            if frame.epoch!=old.epoch or delta>0x8000:
                reason='Contact stream reset'
            elif delta!=1:
                self.gaps+=max(0,delta-1)
                # Small snapshot skips are safe only with unchanged lifetimes.
                if delta>3 or self.contact_ids(frame)!=self.contact_ids(old):
                    reason='Contact continuity lost'
        # A flagged frame explains its epoch change: report one cause, not a
        # fictitious second MCU restart. Preserve all flag bits in raw logs.
        if frame.flags&0x3f:
            reason=', '.join(label for bit,label in (
                (1,'Palm detected'),(2,'Sensor contact limit'),
                (4,'Sensor acquisition invalid'),(8,'Sensor reset / calibration'),
                (16,'Stream discontinuity'),(32,'Split snapshot stale')) if frame.flags&bit)
        if frame.flags&0x2c:self.bad_since.setdefault(frame.pad,now)
        else:self.bad_since.pop(frame.pad,None)
        # An unplugged pad's endless faults must not keep canceling the other pad.
        if reason and self.absent(frame.pad,now):reason=None
        if reason:self.cancel(('Left: ' if frame.pad==0 else 'Right: ')+reason)
        elif frame.flags&0x40:
            # Firmware claimed this gesture: one owner, so Windows lets go now.
            if self.ready:
                self.claims+=1;self.cancel('Firmware gesture',fault=False)
            self.reason='Firmware gesture · lift all fingers to rearm'
        self.samples[frame.pad]=(frame,now)

    def absent(self,pad,now):
        since=self.bad_since.get(pad)
        return since is not None and now-since>ABSENT_S

    def live_pads(self,now):
        """The pads taking part now: a fresh heartbeat and not persistently faulted."""
        return tuple(p for p in (0,1) if p in self.samples and 0<=now-self.samples[p][1]<=.06 and not self.absent(p,now))

    def merge(self,raw,now):
        """Hold back a contact that lands on the other pad too late to be part of this touch."""
        if not raw:
            self.session_pads=set();self.session_start=None;self.held=set();return raw
        for key in sorted(raw,key=lambda k:k[0]):
            if key in self.identities or key in self.held:continue
            pad=key[0]
            if not self.session_pads:self.session_pads={pad};self.session_start=now
            elif pad in self.session_pads:pass
            elif self.always_merge or now-self.session_start<=self.merge_window:self.session_pads.add(pad)
            else:self.held.add(key)
        self.held&=set(raw)
        return {k:v for k,v in raw.items() if k not in self.held}

    @staticmethod
    def contact_ids(frame):
        return {c[0] for i,c in enumerate(frame.contacts) if frame.mask&(1<<i)}

    def raw_points(self,pads=None):
        result={}
        for pad,(frame,_) in self.samples.items():
            if pads is not None and pad not in pads:continue
            if frame.flags&0x3f:continue
            for key,(x,y) in frame.himetric().items():
                if self.flips[pad][0]:x=4300-x
                if self.flips[pad][1]:y=4000-y
                result[(pad,frame.epoch,key)]=(x+pad*(4300+self.gap),y)
        return result

    def tick(self,now=None):
        now=self.clock() if now is None else now
        if self.last_tick is not None and now-self.last_tick>.1:
            self.cancel('Host input worker stalled')
        self.last_tick=now
        live=self.live_pads(now)
        if live!=self.present:
            # A pad dropped out or came back: start over from all-up on what's left.
            if self.present is not None and self.ready:
                self.cancel('Pad heartbeat missing' if len(live)<len(self.present) else 'Pad came back',fault=len(live)<len(self.present))
            self.present=live
        if not live:
            self.reason='Waiting for the pads';return None
        frames=[self.samples[p][0] for p in live]
        times=[self.samples[p][1] for p in live]
        if any(f.flags&0x7f for f in frames):return None
        if len(live)==2 and abs(times[0]-times[1])>.025:
            if self.ready and abs(times[0]-times[1])>.05:self.cancel('Pads out of sync')
            return None
        if not self.ready:
            if any(f.mask for f in frames):return None
            self.ready=True;self.reason='Ready · one continuous pad' if len(live)==2 else ('Ready · left pad only' if live==(0,) else 'Ready · right pad only')
        raw=self.merge(self.raw_points(live),now)
        if self.withhold_fingers:
            counts=Counter(key[0] for key in raw)
            fingers=len(raw) if self.span else max(counts.values(),default=0)
            if fingers>=self.withhold_fingers:
                self.withheld+=1
                self.cancel(f'{fingers}-finger swipe belongs to the firmware',fault=False);return None
        if len(raw)>5:
            self.cancel('Six or more contacts exceed the Windows five-contact limit');return None
        # Stable synthetic IDs are independent of both source lifetime-ID spaces.
        for key in list(self.identities):
            if key not in raw:del self.identities[key]
        for key in raw:
            if key not in self.identities:
                if self.next_id>65535:
                    self.cancel('Synthetic contact ID rollover');return None
                self.identities[key]=self.next_id;self.next_id+=1
        self.points={self.identities[k]:v for k,v in raw.items()}
        signature=tuple(sorted(self.points.items()))
        if signature!=self.last_output:
            if self.backend:self.backend.frame(self.points)
            self.last_output=signature
        if not self.points:
            self.identities={};self.next_id=1;self.origin=self.metrics=None
        elif len(self.points)==2:
            keys=tuple(sorted(self.points));a,b=(self.points[k] for k in keys)
            if self.origin is None or self.origin[0]!=keys:
                self.origin=(keys,a,b)
            _,oa,ob=self.origin
            base=math.dist(oa,ob)
            if base>100:
                self.metrics=(( (a[0]+b[0]-oa[0]-ob[0])/200,(a[1]+b[1]-oa[1]-ob[1])/200),
                              math.dist(a,b)/base,
                              math.degrees(math.atan2(b[1]-a[1],b[0]-a[0])-math.atan2(ob[1]-oa[1],ob[0]-oa[0])))
        else:self.origin=self.metrics=None
        self.reason=f'{len(self.points)} contacts · one continuous pad' if self.points else 'Ready · touch either or both pads'
        return dict(self.points)
