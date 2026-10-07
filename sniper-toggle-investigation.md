# Sniper Key Toggle Implementation for Svalboard

## Problem Statement

Users want the ability to **toggle** sniper mode (reduced DPI for precision pointing) rather than having to **hold** a key. Currently, sniper keys work as momentary hold keys—you hold to reduce DPI, release to return to normal. A toggle would let users tap once to enter sniper mode and tap again to exit.

## Repository

https://github.com/svalboard/vial-qmk

Branch: `vial`

---

## Real-World Implementations to Reference

### 1. Bastard Keyboards Dilemma

**Documentation:** https://docs.bastardkb.com/fw/dilemma-features.html

The Dilemma has a mature sniper implementation with both hold and toggle options:

```c
// API functions available:
dilemma_set_pointer_sniping_enabled(bool enable)  // enable/disable sniping mode
dilemma_get_pointer_sniping_enabled()             // returns whether sniping is currently enabled
dilemma_cycle_pointer_sniping_dpi(bool forward)   // cycle through DPI presets
dilemma_cycle_pointer_sniping_dpi_noeeprom(bool forward)  // cycle without EEPROM persistence
dilemma_get_pointer_sniping_dpi()                 // returns current sniping DPI value
```

They also support **auto-sniping on layer**:
```c
#define DILEMMA_AUTO_SNIPING_ON_LAYER LAYER_POINTER
```

This automatically enables sniper mode when a specific layer is active—could be an alternative UX pattern.

**Key insight:** They maintain separate DPI values for "default" and "sniping" modes, both adjustable at runtime.

### 2. HolyKeebs Firmware

**Documentation:** https://docs.holykeebs.com/firmware/

HolyKeebs implements sniper as a **scale profile** rather than changing CPI/DPI directly:

> "The current implementation intentionally doesn't alter the related CPI/DPI setting as this doesn't work well with all pointing devices (e.g. on a touchpad where a lot of different settings are derived from the CPI and cease to function properly). Sniping is simply another scale profile that can be applied to a pointing device, either by holding a key or toggling the mode."

They explicitly support **both hold and toggle**:
- Hold a key to activate sniping profile
- Toggle the mode for persistent sniping

They also note that holding Shift while using config keycodes affects the peripheral pointing device (for split keyboards).

### 3. QMK Core - Key Lock Feature

**Documentation:** https://docs.qmk.fm/features/key_lock

QMK has a built-in `QK_LOCK` keycode:
- Press `QK_LOCK`, then press any key
- That key stays "held" until pressed again
- Similar to Caps Lock but for arbitrary keys

**Limitation:** Only works with basic keycodes (0x00-0xFF range). Custom keycodes with internal firmware logic won't work because Key Lock uses `register_code()`/`unregister_code()` which only handles HID keycodes.

```c
// From quantum/process_keycode/process_key_lock.c
// Uses a 256-bit array to track locked keys - only basic keycodes
```

**Verdict:** Won't work for sniper if it's a custom keycode, but worth checking if Svalboard's sniper happens to use a basic keycode.

### 4. QMK Pointing Device - Drag Scroll Example

**Documentation:** https://docs.qmk.fm/features/pointing_device

QMK docs show a toggle pattern for drag scroll that's directly applicable:

```c
enum custom_keycodes {
    DRAG_SCROLL = SAFE_RANGE,
};

bool set_scrolling = false;

report_mouse_t pointing_device_task_user(report_mouse_t mouse_report) {
    if (set_scrolling) {
        mouse_report.h = mouse_report.x;
        mouse_report.v = mouse_report.y;
        mouse_report.x = 0;
        mouse_report.y = 0;
    }
    return mouse_report;
}

bool process_record_user(uint16_t keycode, keyrecord_t *record) {
    if (keycode == DRAG_SCROLL && record->event.pressed) {
        set_scrolling = !set_scrolling;  // Toggle on press
    }
    return true;
}
```

This same pattern applies directly to sniper mode—just toggle a boolean and check it in `pointing_device_task_user()` to scale the mouse report.

### 5. Vial Custom Keycodes Pattern

**Reference:** https://get.vial.today/docs/porting-to-vial.html

For Vial compatibility, custom keycodes should use the `QK_KB_0` through `QK_KB_31` range:

```c
enum my_keycodes {
    MY_CUSTOM_KEY = QK_KB_0,
    SNIPER_TOGGLE = QK_KB_1,
    // etc.
};
```

Then expose in `vial.json`:
```json
{
    "customKeycodes": [
        {"name": "Sniper Toggle", "title": "Toggle sniper mode on/off", "shortName": "SNPTG"}
    ]
}
```

These appear in Vial's "User" tab and can be assigned to any key without reflashing.

---

## Investigation Tasks

1. **Find the sniper key implementation**
   - Look in `keyboards/svalboard/` for:
     - `svalboard.c` - main keyboard code
     - `keycodes.h` or similar - custom keycode definitions
     - `vial.json` - Vial custom keycode definitions
     - Any pointing device related files
   - Search for terms: `SNIPER`, `sniper`, `sniping`, `DPI`, `pointing_device`

2. **Understand the current implementation**
   - How is sniper mode currently activated? (custom keycode? layer-based?)
   - What function controls the DPI change?
   - Is there already a toggle variant that's just not exposed?

3. **Research QMK patterns**
   - Check `quantum/pointing_device/` for any built-in sniper support
   - Look at how other keyboards implement sniper toggle (e.g., Bastard Keyboards Dilemma uses `dilemma_set_pointer_sniping_enabled()`)

## Solution Approach (Likely)

Based on research, the implementation will probably look like:

```c
// In keycodes enum (or use QK_KB_0 range for Vial compatibility)
enum svalboard_keycodes {
    SV_SNIPER = SAFE_RANGE,      // existing hold-based sniper
    SV_SNIPER_TOGGLE,            // new toggle variant
    // ... other keycodes
};

static bool sniper_locked = false;

bool process_record_user(uint16_t keycode, keyrecord_t *record) {
    switch (keycode) {
        case SV_SNIPER_TOGGLE:
            if (record->event.pressed) {
                sniper_locked = !sniper_locked;
                // Call whatever function currently enables/disables sniper mode
                set_sniper_mode(sniper_locked);  // or similar
            }
            return false;
        // ... existing cases
    }
    return true;
}
```

For Vial visibility, add to `vial.json`:
```json
"customKeycodes": [
    {"name": "Sniper Toggle", "title": "Toggle sniper mode on/off", "shortName": "SNPTG"}
]
```

## Key Constraints

- **Must be Vial-compatible** - users should be able to assign this key without reflashing
- **No code editing required by end users** - this should be a firmware feature
- **Preserve existing hold behavior** - don't break the current sniper key, just add a toggle option

## Alternative Approaches to Consider

### Option A: Dedicated Toggle Keycode (Recommended)
Add a new `SNIPER_TOGGLE` keycode alongside the existing hold-based sniper key. This is what Bastard Keyboards and HolyKeebs do—give users both options.

**Pros:** Clean, explicit, no behavior change to existing key
**Cons:** Uses another keycode slot

### Option B: QMK's KEY_LOCK Feature
`QK_LOCK` locks the next key pressed until pressed again.

**Pros:** Already exists in QMK, no new code needed
**Cons:** Only works with basic keycodes (0x00-0xFF). If sniper uses custom keycode logic, this won't work.

To enable: `KEY_LOCK_ENABLE = yes` in `rules.mk`

### Option C: Layer-Based with Layer Lock
If sniper mode works by activating a layer (e.g., layer 15 with reduced DPI settings), then `QK_LLCK` (Layer Lock) could toggle it.

**Pros:** Uses existing QMK feature
**Cons:** Only works if sniper is layer-based, not keycode-based

### Option D: Auto-Sniping on Layer (Dilemma Pattern)
Instead of a dedicated sniper key, automatically enable sniper when a specific layer is active:
```c
#define DILEMMA_AUTO_SNIPING_ON_LAYER LAYER_POINTER
```

**Pros:** No dedicated key needed, contextual behavior
**Cons:** Different UX than explicit toggle

### Option E: Tap Dance
Make the sniper key dual-function: tap to toggle, hold for momentary.

**Pros:** Single key does both
**Cons:** Adds tap/hold timing complexity, may feel sluggish

### Option F: Scale-Based Approach (HolyKeebs Pattern)
Instead of changing DPI at the sensor level, apply a multiplier in `pointing_device_task_user()`:
```c
report_mouse_t pointing_device_task_user(report_mouse_t mouse_report) {
    if (sniper_enabled) {
        mouse_report.x = mouse_report.x / 4;  // or use float multiplier
        mouse_report.y = mouse_report.y / 4;
    }
    return mouse_report;
}
```

**Pros:** Works with any pointing device, doesn't touch sensor registers
**Cons:** May feel different than true DPI change

---

## Deliverable

A PR-ready implementation that adds a sniper toggle keycode, exposed in Vial's custom keycodes so users can assign it to any key without reflashing.

## Reference URLs

- Bastard Keyboards Dilemma Features: https://docs.bastardkb.com/fw/dilemma-features.html
- HolyKeebs Firmware Docs: https://docs.holykeebs.com/firmware/
- QMK Pointing Device Docs: https://docs.qmk.fm/features/pointing_device
- QMK Key Lock Docs: https://docs.qmk.fm/features/key_lock
- Vial Porting Guide (custom keycodes): https://get.vial.today/docs/porting-to-vial.html
