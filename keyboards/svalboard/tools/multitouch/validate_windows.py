"""Does Windows deliver each touchpad feature from the lab's injected input?

No board needed. Opens its own window, puts the cursor over it, and injects
through the same synthetic touchpad the unified lab uses (WindowsTouchpad,
86 x 40 mm, not registered as touchpad-aware: the window acts like any app).
Then it reports what Windows turned each gesture into:

  tap             -> a left click             (Windows "tap to click")
  one-finger move -> cursor motion
  tap and drag    -> button down, motion with the button held, button up
  two-finger tap  -> a right click
  two-finger move -> wheel messages           (Windows "drag two fingers to scroll")
  pinch           -> Ctrl+wheel, or a zoom    (Windows "pinch to zoom")

Three-finger swipes are system gestures (Task View, desktops, show desktop):
they act on the whole desktop, not this window, so they are only run with
--three-finger, which swipes up (Task View), checks that the foreground
window changed, and presses Esc to close Task View again.

It stops at once if the window loses the foreground, and puts the cursor and
the foreground window back. Run with Windows Python:  python validate_windows.py
"""
import argparse
import ctypes as C
import json
import time

from windows_touchpad import WindowsTouchpad, PTR, I32, U32, Point, user32

WM_MOUSEMOVE, WM_LBUTTONDOWN, WM_LBUTTONUP, WM_RBUTTONDOWN, WM_RBUTTONUP = 0x200, 0x201, 0x202, 0x204, 0x205
WM_MOUSEWHEEL, WM_MOUSEHWHEEL, WM_GESTURE = 0x20A, 0x20E, 0x119
MK_LBUTTON, MK_CONTROL = 0x1, 0x8
W, H = 8600, 4000  # the lab's surface, HIMETRIC (0.01 mm)


def run(three_finger=False):
    import tkinter as tk
    dll = user32()
    dll.GetForegroundWindow.restype = PTR
    dll.SetForegroundWindow.argtypes = [PTR]
    dll.GetAncestor.argtypes, dll.GetAncestor.restype = [PTR, U32], PTR
    dll.GetCursorPos.argtypes = [C.POINTER(Point)]
    dll.SetCursorPos.argtypes = [I32, I32]
    dll.CallWindowProcW.argtypes = [PTR, PTR, U32, C.c_size_t, C.c_ssize_t]
    dll.CallWindowProcW.restype = C.c_ssize_t
    dll.SetWindowLongPtrW.argtypes = [PTR, I32, PTR]
    dll.SetWindowLongPtrW.restype = PTR
    dll.DefWindowProcW.argtypes = [PTR, U32, C.c_size_t, C.c_ssize_t]
    dll.DefWindowProcW.restype = C.c_ssize_t
    dll.GetClassNameW.argtypes = dll.GetWindowTextW.argtypes = [PTR, C.c_wchar_p, I32]
    dll.keybd_event.argtypes = [C.c_ubyte, C.c_ubyte, U32, C.c_size_t]
    old_foreground = dll.GetForegroundWindow()
    old_cursor = Point()
    dll.GetCursorPos(C.byref(old_cursor))

    root = tk.Tk()
    root.title("Unified touchpad: Windows delivery check")
    root.geometry("1000x620+80+80")
    label = tk.Label(root, text="Checking what Windows does with injected touchpad input.\n"
                     "Hands off the mouse for a few seconds. This window closes by itself.",
                     font=("Segoe UI", 15))
    label.pack(expand=True, fill="both")
    root.update()
    hwnd = dll.GetAncestor(root.winfo_id(), 2)
    targets = [label.winfo_id(), root.winfo_id()]
    events = []
    callback_type = C.WINFUNCTYPE(C.c_ssize_t, PTR, U32, C.c_size_t, C.c_ssize_t)
    old_procs = {}

    @callback_type
    def wndproc(window, message, wparam, lparam):
        if message in (WM_MOUSEMOVE, WM_LBUTTONDOWN, WM_LBUTTONUP, WM_RBUTTONDOWN, WM_RBUTTONUP,
                       WM_MOUSEWHEEL, WM_MOUSEHWHEEL, WM_GESTURE):
            events.append((time.monotonic(), message, wparam & 0xFFFF, C.c_int16((wparam >> 16) & 0xFFFF).value))
        proc = old_procs.get(window)
        if not proc:  # a message while the subclass is being installed
            return dll.DefWindowProcW(window, message, wparam, lparam)
        return dll.CallWindowProcW(proc, window, message, wparam, lparam)

    def pump(seconds):
        until = time.monotonic() + seconds
        while time.monotonic() < until:
            root.update()
            time.sleep(.002)

    def center():
        dll.SetCursorPos(root.winfo_rootx() + 500, root.winfo_rooty() + 310)

    def guard():
        # Never inject where a tap could click another app.
        if dll.GetForegroundWindow() != hwnd:
            raise RuntimeError("The check window lost the foreground; injection stopped")
        at = Point()
        dll.GetCursorPos(C.byref(at))
        x0, y0 = root.winfo_rootx(), root.winfo_rooty()
        if not (x0 + 20 <= at.x <= x0 + root.winfo_width() - 20 and y0 + 20 <= at.y <= y0 + root.winfo_height() - 20):
            raise RuntimeError("The cursor left the check window; injection stopped")

    def play(pad, frames, settle=.6):
        """frames: list of {id: (x, y)}; one every 10 ms, then all fingers up."""
        start = time.monotonic()
        for contacts in frames:
            guard()
            pad.frame(contacts)
            pump(.01)
        pad.frame({})
        pump(settle)
        return [e for e in events if e[0] >= start]

    def line(x0, y0, x1, y1, steps):
        return [(round(x0 + (x1 - x0) * k / steps), round(y0 + (y1 - y0) * k / steps)) for k in range(steps + 1)]

    result = {}
    cx, cy = W // 2, H // 2
    try:
        for t in targets:
            old_procs[t] = dll.SetWindowLongPtrW(t, -4, C.cast(wndproc, PTR))
            if not old_procs[t]:
                raise C.WinError(C.get_last_error())
        dll.SetForegroundWindow(hwnd)
        center()
        pump(.4)
        guard()
        with WindowsTouchpad(width=W, height=H) as pad:
            def kinds(ev):
                return [m for _, m, _, _ in ev]

            # Tap: one finger down for 40 ms.
            center(); ev = play(pad, [{1: (cx, cy)}] * 4, settle=.8)
            result["tap_to_click"] = WM_LBUTTONDOWN in kinds(ev) and WM_LBUTTONUP in kinds(ev)

            # One finger moving 8 mm: cursor motion.
            center(); before = Point(); dll.GetCursorPos(C.byref(before))
            ev = play(pad, [{2: p} for p in line(cx - 400, cy, cx + 400, cy, 30)], settle=.3)
            after = Point(); dll.GetCursorPos(C.byref(after))
            result["cursor_motion"] = after.x != before.x and WM_LBUTTONDOWN not in kinds(ev)

            # Tap and drag: tap, touch again within 150 ms and move, lift.
            center()
            ev = play(pad, [{3: (cx, cy)}] * 4 + [{}] * 8 + [{4: p} for p in line(cx, cy, cx + 600, cy, 30)] + [{4: (cx + 600, cy)}] * 10, settle=.8)
            held = any(m == WM_MOUSEMOVE and lo & MK_LBUTTON for _, m, lo, _ in ev)
            result["tap_and_drag"] = WM_LBUTTONDOWN in kinds(ev) and held and WM_LBUTTONUP in kinds(ev)

            # Two-finger tap: a right click.
            center(); ev = play(pad, [{5: (cx - 500, cy), 6: (cx + 500, cy)}] * 4, settle=.8)
            result["two_finger_tap"] = WM_RBUTTONDOWN in kinds(ev) or WM_RBUTTONUP in kinds(ev)

            # Two fingers moving together 12 mm: scroll.
            center()
            ev = play(pad, [{7: (cx - 500, y), 8: (cx + 500, y)} for _, y in line(0, cy + 600, 0, cy - 600, 40)], settle=.8)
            wheel = [(lo, d) for _, m, lo, d in ev if m == WM_MOUSEWHEEL]
            result["two_finger_scroll"] = any(not lo & MK_CONTROL for lo, _ in wheel)

            # Pinch out, 10 mm on each side: zoom (Ctrl+wheel for ordinary apps).
            center()
            ev = play(pad, [{9: (cx - 600 - s, cy), 10: (cx + 600 + s, cy)} for s, _ in line(0, 0, 1000, 0, 40)], settle=.8)
            wheel = [(lo, d) for _, m, lo, d in ev if m == WM_MOUSEWHEEL]
            result["pinch_zoom"] = any(lo & MK_CONTROL for lo, _ in wheel) or WM_GESTURE in kinds(ev)
            result["pinch_zoom_detail"] = {"ctrl_wheel": sum(1 for lo, _ in wheel if lo & MK_CONTROL),
                                           "plain_wheel": sum(1 for lo, _ in wheel if not lo & MK_CONTROL),
                                           "gesture_messages": kinds(ev).count(WM_GESTURE)}

            if three_finger:
                center(); guard()
                start = dll.GetForegroundWindow()
                for contacts in [{11: (cx - 800, y), 12: (cx, y), 13: (cx + 800, y)} for _, y in line(0, cy + 1200, 0, cy - 1200, 30)]:
                    pad.frame(contacts)
                    pump(.01)
                pad.frame({})
                pump(1.0)
                now_fg = dll.GetForegroundWindow()
                result["three_finger_swipe_up"] = now_fg != start
                if now_fg != start:
                    name, title = C.create_unicode_buffer(256), C.create_unicode_buffer(256)
                    dll.GetClassNameW(now_fg, name, 256); dll.GetWindowTextW(now_fg, title, 256)
                    result["three_finger_foreground"] = f"{name.value} / {title.value}"
                    if "Task" in title.value or "Xaml" in name.value or "Multitasking" in name.value:
                        dll.keybd_event(0x1B, 0, 0, 0); dll.keybd_event(0x1B, 0, 2, 0)  # Esc closes Task View
                        pump(.5)
            else:
                result["three_finger_swipe_up"] = "not run (pass --three-finger)"
        result["passed"] = all(v is True for k, v in result.items() if k not in ("pinch_zoom_detail", "three_finger_swipe_up", "three_finger_foreground")) \
            and result["three_finger_swipe_up"] is not False
        return result
    finally:
        foreground_ours = dll.GetForegroundWindow() == hwnd
        for t, proc in old_procs.items():
            if proc:
                dll.SetWindowLongPtrW(t, -4, proc)
        root.destroy()
        dll.SetCursorPos(old_cursor.x, old_cursor.y)
        if foreground_ours:
            dll.SetForegroundWindow(old_foreground)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--three-finger", action="store_true", help="Also swipe three fingers up (opens Task View)")
    p.add_argument("--output", help="Write the result as JSON here too")
    args = p.parse_args()
    try:
        result = run(args.three_finger)
    except Exception as exc:
        result = {"error": repr(exc), "passed": False}
    text = json.dumps(result, indent=2)
    print(text)
    if args.output:
        with open(args.output, "w", encoding="utf-8") as f:
            f.write(text + "\n")
    return 0 if result.get("passed") else 1


if __name__ == "__main__":
    raise SystemExit(main())
