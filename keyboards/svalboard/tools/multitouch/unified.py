"""Two independently clocked pads mapped onto one physical-size surface.

Never equate source clocks. Freshness uses host receipt time; a bounded latest
sample join tolerates a stationary hand, not a missing hand. No OS dependencies.
"""
import math


class UnifiedPad:
    def __init__(self, backend=None, gap_mm=0, flips=((False,False),(False,False)), clock=None):
        import time
        if not math.isfinite(gap_mm) or not 0<=gap_mm<=50: raise ValueError('Gap must be 0–50 mm')
        self.backend, self.gap, self.flips = backend, gap_mm*100, flips
        self.clock=clock or time.monotonic
        self.width,self.height=8600+self.gap,4000
        self.samples={};self.ready=False;self.points={};self.reason='Lift all fingers on both pads to arm'
        self.cancellations=0;self.gaps=0;self.identities={};self.next_id=1
        self.origin=None;self.metrics=None;self.last_output=None;self.last_tick=None

    def cancel(self,reason):
        if self.backend:self.backend.cancel()
        self.ready=False;self.points={};self.identities={};self.next_id=1
        self.origin=self.metrics=self.last_output=None
        self.reason=reason+' · lift all fingers to rearm';self.cancellations+=1

    def feed(self,frame,received=None):
        now=self.clock() if received is None else received
        previous=self.samples.get(frame.pad)
        if previous:
            old,when=previous
            delta=(frame.sequence-old.sequence)&65535
            if delta==0 and frame.epoch==old.epoch:return
            if now<when or now-when>.06:
                self.cancel('Pad heartbeat interrupted')
            if frame.epoch!=old.epoch or delta>0x8000:
                self.cancel('Pad restarted')
            elif delta!=1:
                self.gaps+=max(0,delta-1)
                # Snapshot transport can skip intermediate reports. Lifetime IDs
                # let a small gap retain existing fingers without guessing lifts.
                if delta>3 or self.contact_ids(frame)!=self.contact_ids(old):
                    self.cancel('Contact continuity lost')
        if frame.flags&0x7f:self.cancel('Sensor / split frame invalid')
        self.samples[frame.pad]=(frame,now)

    @staticmethod
    def contact_ids(frame):
        return {c[0] for i,c in enumerate(frame.contacts) if frame.mask&(1<<i)}

    def raw_points(self):
        result={}
        for pad,(frame,_) in self.samples.items():
            if frame.flags&0x7f:continue
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
        if len(self.samples)!=2:
            self.reason='Waiting for both pads';return None
        frames=[self.samples[p][0] for p in (0,1)]
        times=[self.samples[p][1] for p in (0,1)]
        if any(now-t>.06 or now<t for t in times):
            if self.ready:self.cancel('Pad heartbeat missing')
            return None
        if any(f.flags&0x7f for f in frames):return None
        if abs(times[0]-times[1])>.025:
            if self.ready and abs(times[0]-times[1])>.05:self.cancel('Pads out of sync')
            return None
        if not self.ready:
            if any(f.mask for f in frames):return None
            self.ready=True;self.reason='Ready · one continuous pad'
        raw=self.raw_points()
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
