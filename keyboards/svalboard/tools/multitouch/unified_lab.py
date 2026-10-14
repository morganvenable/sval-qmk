"""Live unified two-pad Windows lab. Preview by default; real input opt-in."""
from dataclasses import asdict
import argparse
import json
from pathlib import Path
import queue
import threading
import time
import traceback
from transport import Assembler,HidDispatcher,StreamClient
from capture import open_device
from unified import UnifiedPad
import gesture_config

REBIND_S=60  # wrapper IDs last two minutes; the lease moves to a fresh one well before


def main():
    import tkinter as tk
    from tkinter import ttk
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--serial',required=True);p.add_argument('--log',type=Path)
    p.add_argument('--smoke',action='store_true',help='Close after two seconds; no real input')
    p.add_argument('--no-manage-windows-settings',action='store_true',
                   help="Leave Windows' touchpad pan/zoom settings alone (by default they follow the firmware's owners for the session)")
    args=p.parse_args();log=args.log.open('x',encoding='utf-8') if args.log else None
    root=tk.Tk();root.title('Unified touchpad · two hands, one surface');root.geometry('1040x825+70+30')
    ttk.Label(root,text='Two touchpads → one multitouch surface',font=('Segoe UI',22)).pack(pady=14)
    ttk.Label(root,text='Left + right, side by side · 86 × 40 mm · up to five combined Windows contacts\nLift all fingers to arm. Then try one finger on each pad and move them apart or together.').pack()
    real=tk.BooleanVar(value=False);gap=tk.DoubleVar(value=0);flips=[tk.BooleanVar(value=False) for _ in range(4)]
    input_error=tk.StringVar(value='')
    status=tk.StringVar(value='Connecting…');details=tk.StringVar(value='');mode_label=tk.StringVar(value='PREVIEW ONLY · ordinary mouse output remains active')
    owners_label=tk.StringVar(value='Gesture owners: reading the firmware…')
    row=ttk.Frame(root);row.pack(pady=12)
    real_button=ttk.Checkbutton(row,text='Enable REAL Windows touchpad input',variable=real)
    real_button.pack(side='left',padx=12)
    ttk.Label(row,text='Virtual gap (mm)').pack(side='left');gapbox=ttk.Spinbox(row,from_=0,to=50,textvariable=gap,width=5);gapbox.pack(side='left')
    orient=ttk.Frame(root);orient.pack()
    for i,text in enumerate(('Flip left X','Flip left Y','Flip right X','Flip right Y')):
        ttk.Checkbutton(orient,text=text,variable=flips[i]).pack(side='left',padx=12)
    ttk.Label(root,textvariable=mode_label,wraplength=1000).pack(pady=8)
    ttk.Label(root,textvariable=owners_label,wraplength=1000).pack()
    ttk.Label(root,textvariable=input_error,foreground='#b03020',wraplength=1000).pack()
    surface=tk.Canvas(root,width=960,height=320,bg='#101c29',highlightthickness=0);surface.pack()
    ttk.Label(root,textvariable=status,font=('Segoe UI',15),wraplength=1000).pack(pady=10)
    ttk.Label(root,textvariable=details,font=('Segoe UI',11)).pack()
    view=tk.Canvas(root,width=960,height=125,bg='#101c29',highlightthickness=0);view.pack(pady=8)
    stop=threading.Event();commands=queue.Queue();display=queue.Queue(maxsize=2);alerts=queue.Queue()
    generation=0
    def configure(*_):
        nonlocal generation
        try:
            g=float(gap.get())
            if not 0<=g<=50:raise ValueError()
        except (ValueError,tk.TclError):g=0;gap.set(0);real.set(False)
        generation+=1
        commands.put((generation,real.get(),g,tuple(v.get() for v in flips)))
        mode_label.set('REAL WINDOWS INPUT · firmware suppresses ordinary mouse output from both halves' if real.get() else 'PREVIEW ONLY · ordinary mouse output remains active')
    real.trace_add('write',configure)
    for v in flips:v.trace_add('write',configure)
    gapbox.config(command=configure);gapbox.bind('<Return>',configure);gapbox.bind('<FocusOut>',configure)
    def publish(item):
        if item[0] in ('fallback','error','owners'):
            alerts.put(item);return
        if display.full():
            try:display.get_nowait()
            except queue.Empty:pass
        display.put_nowait(item)
    def worker():
        device=dispatcher=client=backend=None;active=False;config=None;model=None
        counts=[0,0];last_log=0;gestures=None;owner_bits=None
        settings=None
        if not args.no_manage_windows_settings:
            try:
                from windows_settings import SessionSettings
                settings=SessionSettings(log=lambda m:publish(('owners',None,m)))
                settings.recover()
            except Exception as exc:
                settings=None;publish(('owners',None,f'Windows touchpad settings not managed: {exc}'))
        def read_gestures(enabled):
            nonlocal gestures
            try:gestures=gesture_config.get(client)
            except Exception as exc:
                publish(('owners',gestures,f'Could not read gesture owners ({exc}); keeping {gestures.summary() if gestures else "Windows for all"}'));return
            if model:model.configure(gestures)
            text='Gesture owners: '+(gestures.summary() if gestures else 'every gesture Windows (this firmware has no gesture settings)')
            if settings and enabled:
                want=settings.apply(gestures)
                text+=f' · Windows pan {"on" if want["panEnabled"] else "off"}, zoom {"on" if want["zoomEnabled"] else "off"}'
            publish(('owners',gestures,text))
            if log:log.write(json.dumps(dict(event='gestures',time=time.time(),config=gestures.__dict__ if gestures else None))+'\n');log.flush()
        def advance(frame=None,received=None):
            nonlocal backend,active
            try:
                if frame is not None:model.feed(frame,received)
                model.tick()
                return True
            except Exception as exc:
                if backend is None:raise
                message=f'{type(exc).__name__}: {exc}'
                if log:
                    log.write(json.dumps(dict(event='input_error',time=time.time(),error=message,traceback=traceback.format_exc()))+'\n');log.flush()
                backend.close();backend=None;model.backend=None
                model.cancel('Windows input stopped')
                client.command(3);active=False
                commands.put((gen,False,g,flip)) # restart the lease in preview mode
                publish(('fallback',message))
                return False
        try:
            device=open_device(args.serial);packets=queue.Queue(maxsize=256)
            # Receipt timestamps are taken by the HID I/O thread, never the GUI.
            dispatcher=HidDispatcher(device,lambda packet:packets.put_nowait((packet,time.monotonic())),report_size=64)
            client=StreamClient(dispatcher)
            renewed=identity_at=gestures_at=0
            while not stop.is_set():
                change=False
                while not commands.empty():config=commands.get();change=True
                if config is None:stop.wait(.01);continue
                gen,enabled,g,flip=config
                now=time.monotonic()
                if change:
                    if backend:backend.close();backend=None
                    if active:client.command(3);active=False
                    client.bootstrap();identity_at=now
                    reply=client.command(1,2 if enabled else 0);active=True
                    if enabled and not reply['ownership']&8:raise RuntimeError('Firmware did not grant unified pair capture')
                    while not packets.empty():packets.get_nowait()
                    assembler=Assembler(client.client,client.token)
                    if enabled:
                        from windows_touchpad import WindowsTouchpad
                        backend=WindowsTouchpad(width=8600+round(g*100),height=4000)
                    model=UnifiedPad(backend,g,(flip[:2],flip[2:]));renewed=now
                    owner_bits=reply['ownership']&0x30;read_gestures(enabled);gestures_at=now
                    if settings and settings.active and not enabled:settings.restore()
                    if log:log.write(json.dumps(dict(event='session',time=time.time(),real=enabled,gap=g))+'\n');log.flush()
                if dispatcher.failure:raise dispatcher.failure
                now=time.monotonic()
                if now-identity_at>=REBIND_S:
                    # A plain renew under a fresh wrapper ID: no STOP/START, no cut-off gesture.
                    reply=client.rebind();assembler.rebind(client.client);identity_at=renewed=now
                elif now-renewed>=.1:reply=client.command(2);renewed=now
                else:reply=None
                if (reply and reply['ownership']&0x30!=owner_bits) or now-gestures_at>=2:
                    # The owners changed (Keybard or the CLI sent a SET): re-read and re-apply.
                    # The info flags only carry two bits, so re-read every two seconds as well.
                    if reply:owner_bits=reply['ownership']&0x30
                    read_gestures(enabled);gestures_at=now
                batch=[]
                try:batch.append(packets.get(timeout=.005))
                except queue.Empty:pass
                while not packets.empty() and len(batch)<64:batch.append(packets.get_nowait())
                for packet,received in batch:
                    frame=assembler.accept(packet)
                    if frame:
                        counts[frame.pad]+=1
                        if log:log.write(json.dumps(dict(event="frame",received=received,**asdict(frame)))+"\n")
                        if time.monotonic()-received>.06:
                            model.cancel('Queued input too old');continue
                        # Preserve lifts between UI updates; merge on the input worker.
                        if not advance(frame,received):break
                if enabled and backend is None:continue
                if not advance():continue
                now=time.monotonic()
                if now-last_log>.025:
                    publish((gen,model.raw_points(),model.points,model.width,model.reason,model.metrics,tuple(counts),model.cancellations,model.gaps,sum(model.faults.values()),model.claims,model.withheld))
                    if log:
                        log.write(json.dumps(dict(event='state',time=time.time(),real=enabled,counts=counts,reason=model.reason,contacts=model.points,cancellations=model.cancellations,gaps=model.gaps,faults=dict(model.faults)))+'\n');log.flush()
                    last_log=now
        except Exception as exc:
            if log:log.write(json.dumps(dict(event='error',time=time.time(),error=str(exc),traceback=traceback.format_exc()))+'\n');log.flush()
            publish(('error',str(exc)))
        finally:
            try:
                if backend:backend.close()
            finally:
                if active and dispatcher and not dispatcher.failure:
                    try:client.command(3)
                    except Exception:pass
                if dispatcher:dispatcher.close()
                if device:device.close()
                if settings:
                    try:settings.restore()
                    except Exception as exc:print('Could not restore Windows touchpad settings:',exc)
    def poll():
        while not alerts.empty() or not display.empty():
            item=alerts.get_nowait() if not alerts.empty() else display.get_nowait()
            if item[0]=='owners':
                owners_label.set(item[2]);continue
            if item[0]=='fallback':
                real.set(False);input_error.set('Windows input stopped; preview resumed. '+item[1]);continue
            if item[0]=='error':
                real.set(False);real_button.config(state='disabled');mode_label.set('DISCONNECTED · ordinary pointing restored after lease expiry');status.set(item[1]);continue
            gen,raw,points,width,reason,metrics,counts,cancels,gaps,faults,claims,withheld=item
            if gen!=generation:continue
            surface.delete('all');scale=min(880/width,250/4000);offset=(960-width*scale)/2
            for hand in (0,1):
                x=offset+hand*(width-4300)*scale
                surface.create_rectangle(x,35,x+4300*scale,285,outline=('#71ddc9','#ffb570')[hand],width=2)
                surface.create_text(x+2150*scale,18,text=('LEFT','RIGHT')[hand],fill='white')
            for (pad,epoch,key),(x,y) in raw.items():
                x=offset+x*scale;y=35+y*scale
                surface.create_oval(x-9,y-9,x+9,y+9,fill=('#71ddc9','#ffb570')[pad],outline='')
                surface.create_text(x,y-19,text=f'{"L" if pad==0 else "R"}{key}',fill='white')
            status.set(reason);details.set(f'Frames left {counts[0]} / right {counts[1]}   ·   canceled gestures {cancels} / faults {faults}   ·   firmware claims {claims} / withheld swipes {withheld}   ·   skipped source samples {gaps}')
            view.delete('all')
            import math
            pan,zoom,angle=metrics if metrics else ((0,0),1,0)
            theta=math.radians(angle)
            def point(x,y):return (480+pan[0]*4+zoom*(x*math.cos(theta)-y*math.sin(theta)),62+pan[1]*2+zoom*(x*math.sin(theta)+y*math.cos(theta)))
            view.create_polygon(*[v for x,y in [(-55,-27),(55,-27),(55,27),(-55,27)] for v in point(x,y)],fill='#284658',outline='#71ddc9',width=2)
            view.create_text(12,15,anchor='w',text=f'Preview  pan {pan[0]:+.1f}, {pan[1]:+.1f} mm · zoom {zoom:.2f}× · rotation {angle:+.0f}°',fill='white')
        if not stop.is_set():root.after(25,poll)
    def close():stop.set();root.destroy()
    ttk.Button(root,text='Stop / close — return to ordinary pointing',command=close).pack()
    root.protocol('WM_DELETE_WINDOW',close);configure()
    thread=threading.Thread(target=worker,daemon=True);thread.start();poll()
    if args.smoke:root.after(2000,close)
    root.mainloop();stop.set();thread.join(2)
    if log:log.close()

if __name__=='__main__':main()
