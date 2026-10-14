"""Guided Windows sensor characterization. Observation only; volatile settings.

Each group opens a fresh client lease. Closing the window or losing the worker
stops renewal, so firmware restores five contacts / 10 ms automatically.
"""
import argparse
from collections import Counter
from dataclasses import asdict
import json
from pathlib import Path
import queue
import statistics
import threading
import time
import tkinter as tk
from tkinter import ttk
from capture import open_device
from transport import HidDispatcher, StreamClient, Assembler

GROUPS = [
    ('ONE finger: keep it on the pad, gently moving throughout.', [(1,10,1),(2,10,1),(3,10,1),(5,10,1)]),
    ('THREE fingers: keep all three on the pad, gently moving.', [(3,10,3),(5,10,3),(5,20,3),(5,5,3)]),
]


def configure(client, maximum, period):
    for retry in range(3):
        try:
            result = client.characterize(5, maximum, period)
            result['retries'] = retry
            return result
        except RuntimeError as exc:
            if 'rejected: 4' not in str(exc) or retry == 2:
                raise
            client.command(2)
            time.sleep(.04)


def run_group(serial, phases, directory, stop, messages, seconds):
    packets = queue.Queue(maxsize=256)
    device = dispatcher = None
    active = False
    try:
        device = open_device(serial)
        dispatcher = HidDispatcher(device, packets.put_nowait, report_size=64)
        client = StreamClient(dispatcher)
        client.command(1)
        active = True
        assembler = Assembler(client.client, client.token)
        renewed = time.monotonic()
        for maximum, period, expected in phases:
            if stop.is_set(): break
            configuration = configure(client, maximum, period)
            messages.put(('status', f'Hold {expected} finger(s) · limit {maximum} · requested {period} ms'))
            phase = f'fingers-{expected}_limit-{maximum}_period-{period}'
            frames = []
            started = time.monotonic()
            before = None
            previous = None
            gaps = 0
            with (directory / (phase + '.jsonl')).open('x') as output:
                while not stop.is_set() and time.monotonic() - started < seconds + 1:
                    if dispatcher.failure: raise dispatcher.failure
                    now = time.monotonic()
                    if now - renewed > .1:
                        client.command(2); renewed = now
                    if before is None and now - started >= 1:
                        client.characterize(7)
                        before = client.diagnostics()
                    try: frame = assembler.accept(packets.get(timeout=.005))
                    except queue.Empty: frame = None
                    if frame:
                        messages.put(('contacts', frame.mask.bit_count()))
                        if before is not None:
                            if previous is not None and (frame.sequence - previous) & 65535 != 1: gaps += 1
                            previous = frame.sequence
                            data = dict(received=time.monotonic(), **asdict(frame))
                            frames.append(data); output.write(json.dumps(data)+'\n')
            metrics = client.characterize(6)
            after = client.diagnostics()
            counts = Counter(f['mask'].bit_count() for f in frames if not f['flags'] & 127)
            dt = [((b['timestamp'] - a['timestamp']) & 0xffffffff)/1000 for a,b in zip(frames,frames[1:])]
            errors = {k:after[k]-(before or after)[k] for k in after if k!='max_read_ms'}
            matching = counts[expected]/len(frames) if frames else 0
            result = dict(phase=phase, configuration=configuration, expected_contacts=expected,
                          frames=len(frames), counts=dict(counts), matching_fraction=matching,
                          contact_condition_met=matching>=.8, completed=not stop.is_set(),
                          metrics=metrics, errors=errors, sequence_gaps=gaps,
                          mean_sensor_cycle_ms=metrics['cycle_sum_ms']/max(1,metrics['sensor_samples']),
                          mean_acquisition_ms=metrics['acquisition_sum_ms']/max(1,metrics['sensor_samples']+sum(errors.values())),
                          median_interval_ms=statistics.median(dt) if dt else None,
                          p95_interval_ms=sorted(dt)[int(.95*(len(dt)-1))] if dt else None)
            (directory/(phase+'.json')).write_text(json.dumps(result,indent=2))
            messages.put(('result',result))
            if stop.is_set(): break
    except Exception as exc:
        messages.put(('error',str(exc)))
    finally:
        try:
            if active and not dispatcher.failure:
                configure(client,5,10)
                client.command(3)
        except Exception as exc: messages.put(('error',f'Lease will restore defaults: {exc}'))
        finally:
            try:
                if dispatcher: dispatcher.close()
            except Exception as exc: messages.put(('error',str(exc)))
            finally:
                try:
                    if device: device.close()
                finally: messages.put(('done',None))


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--serial',required=True)
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--seconds',type=float,default=4)
    p.add_argument('--idle',action='store_true',help='Unattended no-contact sweep; no GUI')
    args=p.parse_args()
    if not 1<=args.seconds<=6: p.error('Each measurement must be 1–6 seconds')
    args.output.mkdir(parents=True,exist_ok=False)
    stop=threading.Event(); messages=queue.Queue()
    if args.idle:
        run_group(args.serial,[(n,10,0) for n in (1,2,3,5)],args.output,stop,messages,args.seconds)
        failed=False
        while not messages.empty():
            kind,value=messages.get(); print(kind,json.dumps(value)); failed|=kind=='error'
        raise SystemExit(int(failed))
    root=tk.Tk();root.title('Azoteq · guided timing test');root.geometry('720x390+100+100')
    index=0;worker=None;failed=False
    title=tk.StringVar(value=GROUPS[0][0]);status=tk.StringVar(value='Place your fingers, then click Start group.');contacts=tk.StringVar(value='')
    ttk.Label(root,textvariable=title,font=('Segoe UI',17),wraplength=680).pack(pady=24)
    ttk.Label(root,textvariable=status,font=('Segoe UI',13),wraplength=680).pack(pady=10)
    ttk.Label(root,textvariable=contacts,font=('Segoe UI',26)).pack(pady=15)
    def begin():
        nonlocal worker
        button.config(state='disabled')
        worker=threading.Thread(target=run_group,args=(args.serial,GROUPS[index][1],args.output,stop,messages,args.seconds),daemon=True)
        worker.start()
    button=ttk.Button(root,text='Start group',command=begin);button.pack(pady=10)
    ttk.Label(root,text='Two groups · roughly 20 seconds each · keep fingers down during each group.').pack(pady=10)
    def poll():
        nonlocal index, failed
        while not messages.empty():
            kind,value=messages.get()
            if kind=='status':status.set(value)
            elif kind=='contacts':contacts.set(f'{value} contact(s) detected')
            elif kind=='error':
                failed=True;status.set('Error: '+value)
            elif kind=='done':
                if failed:
                    title.set('Test stopped — see error below.');button.config(state='disabled');continue
                index+=1
                if index<len(GROUPS):
                    title.set(GROUPS[index][0]);status.set('Lift off, reposition, then click Start group.');button.config(state='normal')
                else:
                    title.set('Capture complete — you can lift off.');status.set('Saved results; five-contact / 10 ms defaults restored.');button.config(text='Done',state='disabled')
        root.after(50,poll)
    def close():stop.set();root.destroy()
    root.protocol('WM_DELETE_WINDOW',close);poll();root.mainloop()
    stop.set()
    if worker:worker.join(2)


if __name__=='__main__':main()
