"""Interactive two-pad / two-ball simulation using the production-candidate math.

No hardware access or Windows input injection. Drag the dots on either pad;
presets animate both sources. The checkbox represents a future physical clutch.
"""
import argparse
import math
import time
import tkinter as tk
from tkinter import ttk
from gestures import BallVectors, Sample, TwoHand


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--smoke', action='store_true', help='Exercise a preset, then close')
    args = parser.parse_args()
    root = tk.Tk(); root.title('Two-hand manipulation · simulated sources')
    root.geometry('980x760+80+50')
    held = tk.BooleanVar(value=False); mode = tk.StringVar(value='Touchpads')
    status = tk.StringVar(); metrics = tk.StringVar(value='Pan 0 / 0 mm     Zoom 1.00×     Rotation 0°')
    ttk.Label(root, text='One contact per hand → pan, pinch and rotation', font=('Segoe UI',20)).pack(pady=12)
    ttk.Label(root, text='SIMULATION · no hardware reads or desktop input · drag each dot, or run a preset').pack()
    controls = ttk.Frame(root); controls.pack(pady=10)
    engine = TwoHand(); balls = BallVectors(1)
    positions = [[21.5,20.], [21.5,20.]]; previous = [p[:] for p in positions]
    down = [True, True]; sequences = [0,0]; animation = None; output = None

    def clutch():
        nonlocal output
        engine.set_clutch(held.get()); balls.set_clutch(held.get()); output = None
        for h in (0,1): previous[h] = positions[h][:]

    ttk.Checkbutton(controls,text='Clutch held',variable=held,command=clutch).pack(side='left',padx=10)
    selector = ttk.Combobox(controls,textvariable=mode,values=['Touchpads','Trackballs'],state='readonly',width=12)
    selector.pack(side='left',padx=10)
    def reset(event=None):
        nonlocal engine, balls, animation, output
        held.set(False); engine=TwoHand(); balls=BallVectors(1); animation=None; output=None
        for h in (0,1): positions[h]=[21.5,20.]; previous[h]=positions[h][:]; down[h]=True
    selector.bind('<<ComboboxSelected>>',reset)
    ttk.Button(controls,text='Reset',command=reset).pack(side='left',padx=8)
    padrow = ttk.Frame(root); padrow.pack()
    pads=[]
    for h in (0,1):
        frame=ttk.Frame(padrow); frame.pack(side='left',padx=25)
        ttk.Label(frame,text=('LEFT','RIGHT')[h]+' · 43 × 40 mm').pack()
        canvas=tk.Canvas(frame,width=258,height=240,bg='#142536',highlightthickness=0);canvas.pack()
        pads.append(canvas)
        def drag(event,hand=h):
            nonlocal animation
            animation=None
            positions[hand]=[min(43,max(0,event.x/6)),min(40,max(0,event.y/6))]
        canvas.bind('<Button-1>',drag);canvas.bind('<B1-Motion>',drag)
        def dropout(hand=h):
            down[hand]=False
            root.after(120,lambda: down.__setitem__(hand,True))
        ttk.Button(frame,text='Drop contact / heartbeat for 120 ms',command=dropout).pack(pady=5)
    presets=ttk.Frame(root);presets.pack(pady=5)
    def preset(name):
        nonlocal animation
        reset(); held.set(True); clutch(); animation=(name,time.monotonic())
    for name in ('Pan','Pinch','Rotate'):
        ttk.Button(presets,text=name,command=lambda n=name:preset(n)).pack(side='left',padx=8)
    ttk.Label(root,textvariable=metrics,font=('Segoe UI',15)).pack(pady=8)
    view=tk.Canvas(root,width=880,height=240,bg='#0c1520',highlightthickness=0);view.pack()
    ttk.Label(root,textvariable=status,font=('Segoe UI',11)).pack(pady=8)

    def tick():
        nonlocal animation, output
        now=time.monotonic()
        if animation:
            name,started=animation; amount=math.sin(min(1,(now-started)/2)*math.pi/2)
            if name=='Pan': positions[:]=[[21.5+8*amount,20+4*amount] for _ in (0,1)]
            elif name=='Pinch':positions[:]=[[21.5-6*amount,20],[21.5+6*amount,20]]
            else:
                angle=amount*.7
                positions[:]=[[21.5+10*(1-math.cos(angle)),20-10*math.sin(angle)],
                              [21.5-10*(1-math.cos(angle)),20+10*math.sin(angle)]]
        ball_mode=mode.get()=='Trackballs'
        for h in (0,1):
            if ball_mode:
                if down[h]: balls.feed(h,positions[h][0]-previous[h][0],positions[h][1]-previous[h][1],now)
            else:
                sequences[h]=(sequences[h]+1)&65535
                engine.feed(h,Sample(now,sequences[h],1,{1:tuple(positions[h])} if down[h] else {}))
            previous[h]=positions[h][:]
            c=pads[h];c.delete('all')
            for x in range(0,259,30):c.create_line(x,0,x,240,fill='#263b4e')
            for y in range(0,241,30):c.create_line(0,y,258,y,fill='#263b4e')
            if down[h]:
                x,y=[v*6 for v in positions[h]];c.create_oval(x-10,y-10,x+10,y+10,fill=('#57d9ca','#ffb56b')[h],outline='')
        active=balls.engine if ball_mode else engine
        result=active.snapshot(now)
        if result is not None:output=result
        status.set(active.reason)
        view.delete('all')
        pan,scale,angle=(output.pan,output.scale,output.rotation) if output else ((0,0),1,0)
        metrics.set(f'Pan {pan[0]:+.1f} / {pan[1]:+.1f} mm     Zoom {scale:.2f}×     Rotation {angle:+.1f}°')
        theta=math.radians(angle)
        def point(x,y):return (440+pan[0]*6+scale*(x*math.cos(theta)-y*math.sin(theta)),120+pan[1]*6+scale*(x*math.sin(theta)+y*math.cos(theta)))
        vertices=[point(x,y) for x,y in [(-75,-45),(75,-45),(75,45),(-75,45)]]
        view.create_polygon(*[v for p in vertices for v in p],fill='#284456',outline='#57d9ca',width=3)
        view.create_line(*point(0,0),*point(60,0),fill='#ffb56b',width=4,arrow='last')
        root.after(10,tick)
    tick()
    if args.smoke:
        root.after(50,lambda:preset('Pinch'));root.after(500,root.destroy)
    root.mainloop()


if __name__=='__main__':main()
