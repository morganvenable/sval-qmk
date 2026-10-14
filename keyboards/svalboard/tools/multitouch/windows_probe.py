"""Windows API creation probe, with optional isolated two-finger delivery test."""
import argparse
import ctypes as C
import json
from pathlib import Path
import time

from windows_touchpad import WindowsTouchpad, PointerInfo, Point, PTR, I32, U32, capabilities, user32


def delivery_test():
    import tkinter as tk
    dll = user32()
    dll.GetForegroundWindow.restype = PTR
    dll.SetForegroundWindow.argtypes = [PTR]
    dll.GetAncestor.argtypes, dll.GetAncestor.restype = [PTR, U32], PTR
    dll.GetPointerInfo.argtypes, dll.GetPointerInfo.restype = [U32, C.POINTER(PointerInfo)], I32
    dll.RegisterTouchpadCapableWindow.argtypes = [PTR, I32]
    dll.RegisterTouchpadCapableWindow.restype = I32
    dll.GetCursorPos.argtypes = [C.POINTER(Point)]
    dll.SetCursorPos.argtypes = [I32, I32]
    dll.CallWindowProcW.argtypes = [PTR, PTR, U32, C.c_size_t, C.c_ssize_t]
    dll.CallWindowProcW.restype = C.c_ssize_t
    dll.SetWindowLongPtrW.argtypes = [PTR, I32, PTR]
    dll.SetWindowLongPtrW.restype = PTR
    old_foreground = dll.GetForegroundWindow()
    old_cursor = Point()
    dll.GetCursorPos(C.byref(old_cursor))
    root = tk.Tk()
    root.title("Keybard touchpad prototype — isolated delivery test")
    root.geometry("620x300+100+100")
    surface = tk.Label(root, text="Testing two-finger touchpad input here.\nThis window closes automatically.",
                       font=("Segoe UI", 16))
    surface.pack(expand=True, fill="both")
    root.update()
    hwnd = dll.GetAncestor(root.winfo_id(), 2)
    target = surface.winfo_id()
    result = {"wheel_events": [], "touchpad_pointer_events": [], "injected_frames": 0}
    callback_type = C.WINFUNCTYPE(C.c_ssize_t, PTR, U32, C.c_size_t, C.c_ssize_t)
    old_proc = None

    @callback_type
    def wndproc(window, message, wparam, lparam):
        if message == 0x20A:
            result["wheel_events"].append(C.c_int16((wparam >> 16) & 0xffff).value)
        if message in (0x245, 0x246, 0x247):
            info = PointerInfo()
            if dll.GetPointerInfo(wparam & 0xffff, C.byref(info)) and info.pointerType == 5:
                result["touchpad_pointer_events"].append({"message": message, "id": info.pointerId,
                    "flags": info.pointerFlags, "x": info.ptHimetricLocation.x, "y": info.ptHimetricLocation.y})
        return dll.CallWindowProcW(old_proc, window, message, wparam, lparam)

    def pump(seconds):
        until = time.monotonic() + seconds
        while time.monotonic() < until:
            root.update()
            time.sleep(.002)

    try:
        old_proc = dll.SetWindowLongPtrW(target, -4, C.cast(wndproc, PTR))
        if not old_proc:
            raise C.WinError(C.get_last_error())
        dll.SetForegroundWindow(hwnd)
        dll.SetCursorPos(root.winfo_rootx() + 300, root.winfo_rooty() + 150)
        pump(.3)
        if dll.GetForegroundWindow() != hwnd:
            raise RuntimeError("Test window did not receive foreground; no input injected")
        for pointer_mode in (False, True):
            if not dll.RegisterTouchpadCapableWindow(target, pointer_mode):
                raise C.WinError(C.get_last_error())
            with WindowsTouchpad(gesture_only=True) as pad:
                for step in range(41):
                    if dll.GetForegroundWindow() != hwnd:
                        raise RuntimeError("Test window lost foreground; stopped injection")
                    y = 3200 - step * 55
                    pad.frame({1: (1300, y), 2: (3000, y)})
                    result["injected_frames"] += 1
                    pump(.01)
                pad.frame({})
                result["injected_frames"] += 1
            pump(.3)
        result["scroll_delivered"] = bool(result["wheel_events"])
        result["pointer_delivered"] = bool(result["touchpad_pointer_events"])
        result["passed"] = result["scroll_delivered"] and result["pointer_delivered"]
        return result
    finally:
        foreground_ours = dll.GetForegroundWindow() == hwnd
        if old_proc:
            dll.SetWindowLongPtrW(target, -4, old_proc)
        root.destroy()
        if foreground_ours:
            dll.SetCursorPos(old_cursor.x, old_cursor.y)
            dll.SetForegroundWindow(old_foreground)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--delivery-test", action="store_true")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    result = capabilities()
    try:
        with WindowsTouchpad():
            result["created"] = True
        result["destroyed"] = True
        if args.delivery_test:
            result["delivery"] = delivery_test()
    except Exception as exc:
        result["error"] = repr(exc)
    encoded = json.dumps(result, indent=2)
    print(encoded)
    if args.output:
        args.output.write_text(encoded + "\n", encoding="utf-8")
    return int("error" in result or (args.delivery_test and not result.get("delivery", {}).get("passed")))


if __name__ == "__main__":
    raise SystemExit(main())
