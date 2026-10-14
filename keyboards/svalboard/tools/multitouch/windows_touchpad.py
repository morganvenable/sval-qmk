"""Experimental Windows PT_TOUCHPAD backend, dynamically bound to User32.

Requires Windows; import is portable so the lifecycle code can be unit tested.
Uses POINTER_TOUCH_INFO as specified by Microsoft's Precision Touchpad guide.
Coordinates are device-relative HIMETRIC, not desktop pixels. No SendInput.
"""
import ctypes as C
import sys
import time

U32, I32, U64 = C.c_uint32, C.c_int32, C.c_uint64
PTR = C.c_void_p


class Point(C.Structure):
    _fields_ = [("x", I32), ("y", I32)]


class Rect(C.Structure):
    _fields_ = [(n, I32) for n in ("left", "top", "right", "bottom")]


class PointerInfo(C.Structure):
    _fields_ = [(n, U32) for n in ("pointerType", "pointerId", "frameId", "pointerFlags")] + [
        ("sourceDevice", PTR), ("hwndTarget", PTR),
        ("ptPixelLocation", Point), ("ptHimetricLocation", Point),
        ("ptPixelLocationRaw", Point), ("ptHimetricLocationRaw", Point),
        ("dwTime", U32), ("historyCount", U32), ("InputData", I32),
        ("dwKeyStates", U32), ("PerformanceCount", U64), ("ButtonChangeType", U32)]


class TouchInfo(C.Structure):
    _fields_ = [("pointerInfo", PointerInfo), ("touchFlags", U32), ("touchMask", U32),
                ("rcContact", Rect), ("rcContactRaw", Rect), ("orientation", U32), ("pressure", U32)]


class PenInfo(C.Structure):
    _fields_ = [("pointerInfo", PointerInfo)] + [(n, U32) for n in
                ("penFlags", "penMask", "pressure", "rotation")] + [("tiltX", I32), ("tiltY", I32)]


class PointerUnion(C.Union):
    _fields_ = [("pointerInfo", PointerInfo), ("touchInfo", TouchInfo), ("penInfo", PenInfo)]


class PointerTypeInfo(C.Structure):
    _anonymous_ = ("data",)
    _fields_ = [("type", U32), ("data", PointerUnion)]


class CreationParams(C.Structure):
    _fields_ = [("pointerType", U32), ("maxCount", U32), ("feedbackMode", U32),
                ("hMonitor", PTR), ("deviceWidth", U32), ("deviceHeight", U32), ("options", U32)]


EXPORTS = ("CreateSyntheticPointerDevice2", "InjectSyntheticPointerInput",
           "DestroySyntheticPointerDevice", "RegisterTouchpadCapableWindow", "GetPointerInfo")
PT_TOUCHPAD = 5
CONFIDENCE, INRANGE, INCONTACT = 0x4000, 2, 4


def user32():
    if sys.platform != "win32":
        raise RuntimeError("Run this backend with Windows Python, not WSL Python")
    return C.WinDLL("user32", use_last_error=True)


def capabilities():
    dll = user32()
    return {"windows": str(sys.getwindowsversion()), "pointer_bytes": C.sizeof(PTR),
            "exports": {name: hasattr(dll, name) for name in EXPORTS},
            "abi": {cls.__name__: C.sizeof(cls) for cls in
                    (PointerInfo, TouchInfo, PointerTypeInfo, CreationParams)}}


class WindowsTouchpad:
    """One pad, one calling thread. Contacts are {lifetime_id: (x_hm, y_hm)}.

    Unlike touchscreen injection, touchpad frames describe contact state without
    DOWN/UPDATE/UP flags. Removed contacts are sent once without INCONTACT, at
    their last coordinates. This follows Microsoft's touchpad injection sample.
    """
    def __init__(self, width=4300, height=4000, gesture_only=False):
        if not 1 <= width <= 100000 or not 1 <= height <= 100000:
            raise ValueError("Invalid physical dimensions")
        self.dll = user32()
        for name in EXPORTS[:3]:
            if not hasattr(self.dll, name):
                raise RuntimeError(f"Windows does not export {name}")
        self.create = self.dll.CreateSyntheticPointerDevice2
        self.create.argtypes, self.create.restype = [C.POINTER(CreationParams)], PTR
        self.inject = self.dll.InjectSyntheticPointerInput
        self.inject.argtypes, self.inject.restype = [PTR, C.POINTER(PointerTypeInfo), U32], I32
        self.destroy = self.dll.DestroySyntheticPointerDevice
        self.destroy.argtypes, self.destroy.restype = [PTR], None
        self.width, self.height = width, height
        self.previous = {}
        self.last_ms = 0
        self.started = time.monotonic()
        self.params = CreationParams(PT_TOUCHPAD, 5, 3, None, width, height, 1 | (2 if gesture_only else 0))
        self.handle = self.create(C.byref(self.params))
        if not self.handle:
            raise C.WinError(C.get_last_error())

    def _submit(self, contacts, releases):
        states = [(key, pos, True) for key, pos in contacts.items()] + [
            (key, pos, False) for key, pos in releases.items()]
        if not states:
            return
        reports = (PointerTypeInfo * len(states))()
        self.last_ms = max(self.last_ms + 1, int((time.monotonic() - self.started) * 1000) + 1)
        for report, (key, (x, y), active) in zip(reports, states):
            report.type = PT_TOUCHPAD
            info = report.touchInfo.pointerInfo
            info.pointerType, info.pointerId = PT_TOUCHPAD, key
            info.pointerFlags = CONFIDENCE | (INRANGE | INCONTACT if active else 0)
            info.ptHimetricLocation = Point(x, y)
            info.dwTime = self.last_ms
        if not self.inject(self.handle, reports, len(reports)):
            raise C.WinError(C.get_last_error())

    def frame(self, contacts):
        if not self.handle:
            raise RuntimeError("Touchpad is closed")
        contacts = dict(contacts)
        if len(contacts) > 5 or any(type(k) is not int or not 0 <= k <= 0xffff for k in contacts):
            raise ValueError("Invalid contact IDs/count")
        for x, y in contacts.values():
            if not (0 <= x <= self.width and 0 <= y <= self.height):
                raise ValueError("Contact outside device")
        releases = {key: pos for key, pos in self.previous.items() if key not in contacts}
        # Simultaneous replacement can otherwise exceed the five-report limit.
        if len(contacts) + len(releases) > 5:
            retained = {key: contacts[key] for key in self.previous if key in contacts}
            self._submit(retained, releases)
            self.previous = retained
            releases = {}
            time.sleep(.001)
        self._submit(contacts, releases)
        self.previous = contacts

    def cancel(self):
        # A transport failure is not a physical lift: do not synthesize a tap
        # by sending normal releases. Recreate the device before another gesture.
        if self.previous:
            self.close()
            self.handle = self.create(C.byref(self.params))
            if not self.handle:
                raise C.WinError(C.get_last_error())
            self.started, self.last_ms = time.monotonic(), 0

    def close(self):
        if self.handle:
            self.destroy(self.handle)
            self.handle = None
            self.previous = {}

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()
