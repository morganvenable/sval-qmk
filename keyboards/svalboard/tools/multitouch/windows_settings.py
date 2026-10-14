"""Session-scoped Windows touchpad settings, matched to the firmware's owners.

Windows has no per-device switch for its touchpad gestures, so when the chip
or the firmware owns scroll or pinch, Windows must stop doing it too. For the
length of a session this sets, through SystemParametersInfo
SPI_GETTOUCHPADPARAMETERS (0xAE) / SPI_SETTOUCHPADPARAMETERS (0xAF):

- allowActiveWhenMousePresent = 1 (with it off the synthetic pad goes dead),
- panEnabled  = scroll owner is Windows,
- zoomEnabled = pinch owner is Windows,

and puts the originals back afterward. The originals are saved to a file
first, so a crash is repaired at the next start. The logic is portable; only
Win32Api touches Windows.
"""
import ctypes as C
import json
import os
from pathlib import Path
import sys

U32 = C.c_uint32
SPI_GETTOUCHPADPARAMETERS, SPI_SETTOUCHPADPARAMETERS = 0xAE, 0xAF
SPIF_UPDATEINIFILE, SPIF_SENDCHANGE = 1, 2
MANAGED = ("allowActiveWhenMousePresent", "panEnabled", "zoomEnabled")


class TouchpadParameters(C.Structure):
    """TOUCHPAD_PARAMETERS, version 1 (winuser.h, Windows 11 24H2 SDK)."""
    _fields_ = [("versionNumber", U32), ("maxSupportedContacts", U32), ("legacyTouchpadFeatures", U32),
                ("touchpadPresent", U32, 1), ("legacyTouchpadPresent", U32, 1), ("externalMousePresent", U32, 1),
                ("touchpadEnabled", U32, 1), ("touchpadActive", U32, 1), ("feedbackSupported", U32, 1),
                ("clickForceSupported", U32, 1), ("reserved1", U32, 25),
                ("allowActiveWhenMousePresent", U32, 1), ("feedbackEnabled", U32, 1), ("tapEnabled", U32, 1),
                ("tapAndDragEnabled", U32, 1), ("twoFingerTapEnabled", U32, 1), ("rightClickZoneEnabled", U32, 1),
                ("mouseAccelSettingHonored", U32, 1), ("panEnabled", U32, 1), ("zoomEnabled", U32, 1),
                ("scrollDirectionReversed", U32, 1), ("reserved2", U32, 22),
                ("sensitivityLevel", U32), ("cursorSpeed", U32), ("feedbackIntensity", U32), ("clickForce", U32),
                ("rightClickZoneWidth", U32), ("rightClickZoneHeight", U32)]


class Win32Api:
    def __init__(self):
        if sys.platform != "win32":
            raise RuntimeError("Windows touchpad settings need Windows Python")
        self.spi = C.WinDLL("user32", use_last_error=True).SystemParametersInfoW
        self.spi.argtypes, self.spi.restype = [U32, U32, C.c_void_p, U32], C.c_int

    def read(self):
        p = TouchpadParameters(versionNumber=1)
        if not self.spi(SPI_GETTOUCHPADPARAMETERS, C.sizeof(p), C.byref(p), 0):
            raise C.WinError(C.get_last_error())
        return p

    def write(self, p):
        if not self.spi(SPI_SETTOUCHPADPARAMETERS, C.sizeof(p), C.byref(p), SPIF_UPDATEINIFILE | SPIF_SENDCHANGE):
            raise C.WinError(C.get_last_error())


def default_backup_path():
    base = os.environ.get("LOCALAPPDATA") or str(Path.home())
    return Path(base) / "Svalboard" / "unified-touchpad-settings.json"


class SessionSettings:
    """apply() for the owners in force; restore() when the session ends."""
    def __init__(self, api=None, backup=None, log=print):
        self.api = api or Win32Api()
        self.backup = Path(backup) if backup else default_backup_path()
        self.log = log
        self.active = False

    def recover(self):
        """A backup left by a crashed session: put those settings back first."""
        if self.backup.exists():
            self.log(f"Restoring Windows touchpad settings left by an earlier session ({self.backup})")
            self._restore_from(json.loads(self.backup.read_text(encoding="utf-8")))
            self.backup.unlink()

    def wanted(self, config):
        return {"allowActiveWhenMousePresent": 1,
                "panEnabled": int(config is None or not config.chip_scroll),
                "zoomEnabled": int(config is None or not config.firmware_pinch)}

    def apply(self, config):
        current = self.api.read()
        if not self.active:
            original = {name: int(getattr(current, name)) for name in MANAGED}
            self.backup.parent.mkdir(parents=True, exist_ok=True)
            self.backup.write_text(json.dumps(original), encoding="utf-8")
            self.active = True
        want = self.wanted(config)
        if any(int(getattr(current, n)) != v for n, v in want.items()):
            for name, value in want.items():
                setattr(current, name, value)
            self.api.write(current)
            self.log("Windows touchpad: " + ", ".join(f"{n}={v}" for n, v in want.items()))
        return want

    def restore(self):
        if not self.active:
            return
        self._restore_from(json.loads(self.backup.read_text(encoding="utf-8")))
        self.backup.unlink()
        self.active = False

    def _restore_from(self, original):
        current = self.api.read()
        if any(int(getattr(current, n)) != int(v) for n, v in original.items() if n in MANAGED):
            for name, value in original.items():
                if name in MANAGED:
                    setattr(current, name, int(value))
            self.api.write(current)
        self.log("Windows touchpad settings restored: " + ", ".join(f"{n}={v}" for n, v in original.items()))


if __name__ == "__main__":
    # Read-only report of what this machine has now.
    p = Win32Api().read()
    print({name: int(getattr(p, name)) for name, *_ in TouchpadParameters._fields_ if not name.startswith("reserved")})
