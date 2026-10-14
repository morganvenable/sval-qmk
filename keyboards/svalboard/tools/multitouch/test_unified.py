import unittest
from transport import Frame
from unified import UnifiedPad
from gesture_config import GestureConfig


def f(pad,seq,points=(),epoch=1,flags=0):
    contacts=tuple((key,x,y,10,1) for key,x,y in points)
    return Frame(pad,flags,(1<<len(contacts))-1,seq,seq*10000,epoch,contacts+((0,0,0,0,0),)*(5-len(contacts)))

class Backend:
    def __init__(self):self.frames=[];self.cancels=0
    def frame(self,contacts):self.frames.append(dict(contacts))
    def cancel(self):self.cancels+=1

class Tests(unittest.TestCase):
    def setUp(self):
        self.now=0;self.backend=Backend();self.pad=UnifiedPad(self.backend,clock=lambda:self.now)
        self.pad.feed(f(0,0));self.pad.feed(f(1,0));self.pad.tick()
    def send(self,a,b):
        self.now+=.01;self.pad.feed(a);self.pad.feed(b);return self.pad.tick()
    def test_same_source_ids_map_to_distinct_stable_contacts(self):
        p=self.send(f(0,1,[(1,1024,896)]),f(1,1,[(1,1024,896)]))
        self.assertEqual(len(p),2);self.assertEqual(sorted(p.values()),[(2150,2000),(6450,2000)])
        keys=set(p)
        p=self.send(f(0,2,[(1,1100,896)]),f(1,2,[(1,1024,896)]));self.assertEqual(set(p),keys)
    def test_one_finger_each_can_pinch(self):
        self.send(f(0,1,[(1,1024,896)]),f(1,1,[(1,1024,896)]))
        self.send(f(0,2,[(1,800,896)]),f(1,2,[(1,1250,896)]))
        self.assertGreater(self.pad.metrics[1],1)
    def test_stationary_contacts_remain_when_fresh(self):
        self.send(f(0,1,[(1,1024,896)]),f(1,1,[(1,1024,896)]))
        self.now+=.01;self.pad.feed(f(0,2,[(1,1400,896)]));self.assertEqual(len(self.pad.tick()),2)
    def test_missing_pad_cancels_and_requires_all_up(self):
        self.send(f(0,1,[(1,1024,896)]),f(1,1,[(1,1024,896)]))
        self.now+=.07;self.pad.tick();self.assertFalse(self.pad.ready)
        self.send(f(0,2,[(1,1100,896)]),f(1,2,[(1,1100,896)]));self.assertFalse(self.pad.ready)
        self.send(f(0,3),f(1,3));self.assertTrue(self.pad.ready)
    def test_invalid_frame_never_becomes_normal_release(self):
        self.send(f(0,1,[(1,1024,896)]),f(1,1))
        n=len(self.backend.frames)
        self.send(f(0,2,flags=4),f(1,2));self.assertEqual(len(self.backend.frames),n)
        self.assertGreater(self.backend.cancels,0)
    def test_small_gap_keeps_only_same_lifetimes(self):
        self.send(f(0,1,[(1,100,100)]),f(1,1))
        self.send(f(0,3,[(1,200,100)]),f(1,2));self.assertTrue(self.pad.ready)
        self.send(f(0,5,[(2,200,100)]),f(1,3));self.assertFalse(self.pad.ready)
    def test_restart_cancels_even_same_id(self):
        self.send(f(0,1,[(1,100,100)]),f(1,1))
        self.send(f(0,2,[(1,100,100)],epoch=2),f(1,2));self.assertFalse(self.pad.ready)
    def test_six_contacts_cancel_instead_of_truncate(self):
        fingers=[(i+1,100+i*100,100) for i in range(3)]
        self.send(f(0,1,fingers),f(1,1,fingers));self.assertFalse(self.pad.ready);self.assertFalse(self.pad.points)
    def test_five_contacts_and_real_lift(self):
        p=self.send(f(0,1,[(1,100,100),(2,200,100),(3,300,100)]),f(1,1,[(1,100,100),(2,200,100)]))
        self.assertEqual(len(p),5)
        self.send(f(0,2),f(1,2));self.assertEqual(self.backend.frames[-1],{})
    def test_duplicate_cannot_extend_freshness(self):
        self.send(f(0,1),f(1,1));self.now+=.07;self.pad.feed(f(0,1));self.pad.feed(f(1,1));self.pad.tick();self.assertFalse(self.pad.ready)
    def test_idle_fault_counts_once_despite_epoch_change(self):
        self.send(f(0,1,epoch=2,flags=4),f(1,1))
        self.assertEqual(self.pad.cancellations,0)
        self.assertEqual(dict(self.pad.faults),{'Left: Sensor acquisition invalid':1})
        self.assertEqual(self.backend.cancels,0)
    def test_active_fault_cancels_once_and_distinguishes_split(self):
        self.send(f(0,1,[(1,100,100)]),f(1,1))
        self.send(f(0,2,epoch=2,flags=32),f(1,2,epoch=2,flags=4))
        self.assertEqual(self.pad.cancellations,1)
        self.assertEqual(self.backend.cancels,1)
        self.assertEqual(self.pad.faults['Left: Split snapshot stale'],1)
        self.assertEqual(self.pad.faults['Right: Sensor acquisition invalid'],1)
    def test_geometry_flip_and_gap(self):
        p=UnifiedPad(gap_mm=10,flips=((True,True),(False,False)),clock=lambda:0)
        p.feed(f(0,0));p.feed(f(1,0));p.tick()
        p.feed(f(0,1,[(1,0,0)]));p.feed(f(1,1,[(1,0,0)]));out=p.tick()
        self.assertEqual(sorted(out.values()),[(4300,4000),(5300,0)])
    def test_source_sequence_wrap(self):
        self.pad.samples={0:(f(0,65535),0),1:(f(1,65535),0)}
        self.send(f(0,0,[(1,100,100)]),f(1,0));self.assertTrue(self.pad.ready)


class OwnershipTests(unittest.TestCase):
    """One owner per gesture: firmware claims, swipe withholding and the cross-pad merge rule."""
    def setUp(self):
        self.now=0;self.backend=Backend();self.pad=UnifiedPad(self.backend,clock=lambda:self.now)
        self.pad.feed(f(0,0));self.pad.feed(f(1,0));self.pad.tick();self.seq=0
    def send(self,a=(),b=(),flags=(0,0),dt=.01):
        self.now+=dt;self.seq+=1
        self.pad.feed(f(0,self.seq,a,flags=flags[0]));self.pad.feed(f(1,self.seq,b,flags=flags[1]));return self.pad.tick()
    def test_claim_cancels_windows_once_and_rearms_at_all_up(self):
        self.send([(1,500,500),(2,900,500)])
        n=len(self.backend.frames)
        self.send([(1,450,500),(2,950,500)],flags=(0x40,0x40))
        self.assertEqual(self.backend.cancels,1);self.assertEqual(self.pad.claims,1)
        self.assertEqual(sum(self.pad.faults.values()),0)
        self.send([(1,400,500),(2,1000,500)],flags=(0x40,0x40))
        self.assertEqual(len(self.backend.frames),n);self.assertEqual(self.pad.claims,1)
        self.assertFalse(self.pad.ready)
        self.send();self.assertTrue(self.pad.ready)
    def test_firmware_swipe_is_withheld_from_windows(self):
        self.pad.configure(GestureConfig(swipe_owner='firmware'))
        self.send([(1,500,500),(2,900,500)])
        self.assertEqual(len(self.backend.frames[-1]),2)
        self.send([(1,500,500),(2,900,500),(3,1300,500)])
        self.assertFalse(self.pad.ready);self.assertEqual(self.pad.withheld,1)
        n=len(self.backend.frames)
        self.send([(1,400,500),(2,800,500)]);self.assertEqual(len(self.backend.frames),n)
        self.send();self.assertTrue(self.pad.ready)
    def test_windows_swipe_passes_three_fingers(self):
        self.pad.configure(GestureConfig())
        p=self.send([(1,500,500),(2,900,500),(3,1300,500)]);self.assertEqual(len(p),3)
    def test_late_cross_pad_contact_is_held_back(self):
        p=self.send([(1,500,500)]);self.assertEqual(len(p),1)
        for _ in range(20):p=self.send([(1,520,500)])
        p=self.send([(1,540,500)],[(1,500,500)]);self.assertEqual(len(p),1)  # resting finger on the other pad
        self.assertEqual(list(p.values())[0][0],round(540*4300/2048))
        p=self.send([(1,540,500),(2,900,500)],[(1,500,500)]);self.assertEqual(len(p),2)  # same pad still joins
        p=self.send(b=[(1,500,500)]);self.assertEqual(p,{})  # the held contact stays held
        p=self.send();p=self.send(b=[(1,500,500)]);self.assertEqual(len(p),1)  # a new touch starts on that pad
    def test_cross_pad_within_window_and_always_merge(self):
        self.send([(1,500,500)])
        for _ in range(10):self.send([(1,500,500)])  # 100 ms later: inside the 150 ms window
        p=self.send([(1,500,500)],[(1,500,500)]);self.assertEqual(len(p),2)
        self.send();self.pad.configure(GestureConfig(host_always_merge=True))
        self.send([(1,500,500)])
        for _ in range(30):self.send([(1,500,500)])
        p=self.send([(1,500,500)],[(1,500,500)]);self.assertEqual(len(p),2)
    def test_one_pad_firmware_swipe_counts_per_pad(self):
        self.pad.configure(GestureConfig(swipe_owner='firmware',span_pads=False))
        p=self.send([(1,500,500),(2,900,500)],[(1,500,500)]);self.assertEqual(len(p),3)
        self.assertTrue(self.pad.ready)

if __name__=='__main__':unittest.main()
