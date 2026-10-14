import unittest
from transport import Frame
from unified import UnifiedPad


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

if __name__=='__main__':unittest.main()
