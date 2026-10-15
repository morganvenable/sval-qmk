# Pointing tools (experimental)

While a tool key is held or toggled, a pointing module stops moving the cursor and drives a tool instead: each step of travel sends one volume, arrow, tab, task, zoom or undo/redo press, or the displacement from where the key went down sets a scroll speed. The board sends standard keyboard, consumer and mouse reports only, so no host software is involved.

The tools are built only with `SVAL_POINT_CONTROLS=yes` and are not in released firmware:

```
make svalboard/left:sval SVAL_POINT_CONTROLS=yes
make svalboard/right:sval SVAL_POINT_CONTROLS=yes
```

These are the normal auto-detecting images: one per side, with every pointing driver built in. The target architecture and its reasoning are in [docs/design/discrete-pointing.md](../../../docs/design/discrete-pointing.md); this page describes what is built (settings version 5).

## Tools

Four tool slots. Each slot runs one of eight actions:

| ID | Action | Axis | Output |
|---|---|---|---|
| 0 | Volume | vertical | consumer Volume Up / Down |
| 1 | Arrows | both | Right, Left, Up, Down |
| 2 | Tabs | horizontal | Ctrl+Tab / Ctrl+Shift+Tab |
| 3 | Tasks | horizontal | Alt+Tab / Alt+Shift+Tab, with Alt held until the session ends |
| 4 | Zoom | vertical | Ctrl+= / Ctrl+- (US layout) |
| 5 | Undo / Redo | horizontal | Ctrl+Z / Ctrl+Shift+Z |
| 6 | Undo / Redo (Ctrl+Y) | horizontal | Ctrl+Z / Ctrl+Y |
| 7 | Proportional scroll | both | wheel speed proportional to the displacement from where the tool started |

The defaults are slot 1 Volume, 2 Arrows, 3 Tabs and 4 Tasks. The shortcuts are the Windows ones.

Each slot has a horizontal and a vertical speed (travel per step), an optional rate limit (uncapped by default), a reversal tolerance (for Proportional scroll, the center dead zone) and a reverse flag per axis. "Use shared speeds for every tool" is on by default; with it off, a slot can follow the shared speeds or set its own.

## Keys

The keys are in the `QK_KB` block. Each code is `0x7E00 + side * 8 + slot * 2 + mode`: side `0` left pointer, `1` right; slot `0`-`3`; mode `0` hold, `1` toggle.

| Code | Name | Meaning |
|---|---|---|
| `0x7E00`-`0x7E07` | `SV_POINT_L_*_HOLD`, `SV_POINT_L_*_TOGGLE` | left pointer, slots 1-4 |
| `0x7E08`-`0x7E0F` | `SV_POINT_R_*_HOLD`, `SV_POINT_R_*_TOGGLE` | right pointer, slots 1-4 |
| `0x7E10` | `SV_POINT_CANCEL` | stop every tool; an open task switcher is canceled with Escape |
| `0x7E11` | `SV_POINT_TRACE` | print counters to the console |
| `0x7E12` | `SV_POINT_FINER` | halve the travel per step (until the next save) |
| `0x7E13` | `SV_POINT_COARSER` | double the travel per step (until the next save) |

The middle of each slot name (`VOLUME`, `ARROWS`, `TABS`, `TASKS`) is the slot's default action and stays the same when the slot runs another action.

- On each pointer the newest held key wins, then that pointer's toggle. One toggle per pointer: toggling another slot replaces it, toggling the same slot clears it. The two pointers are independent.
- A release is matched by the key's position, so changing layers while a key is held still releases it.
- Only physical keys activate tools. Codes from combos, macros, tap dances and other synthetic events are dropped; mod-tap and layer-tap wrapping are not supported.
- Changing a pointer's tool ends what it was sending and starts the new tool from rest.
- While a Tasks session is open: Enter or a left click accepts, Escape or another click cancels, any other key accepts and then goes through, and 10 s without a step cancels. The other pointer is ignored.
- Held modifiers block the key-based tools, so Shift+Arrows doesn't select yet. Proportional scroll isn't blocked.

## Motion

- Counts are normalized to 800 CPI before the speeds apply, so a step takes the same travel at any CPI. Trackballs and the trackpad use the CPI set for that hand (the trackpad caps it at its own resolution). A TrackPoint ignores CPI and reports force rather than distance, so its counts are used as they come, as a logical 800 CPI. Each hand's module comes from the same detection `POINTING_HARDWARE` reports; a hand with no module simply produces no steps.
- Sniper raises the travel per step and slows Proportional scroll. Boost is ignored.
- Every tool obeys the board's one free-scroll / axis-lock policy (`pointing/scroll_policy.c`), shared with wheel scrolling on both pointers. It applies while axis lock is on and the host isn't macOS.
- A pointer running a tool doesn't move the cursor and overrides drag-scroll on that pointer; its motion still feeds the shared axis lock. Its motion never turns on the auto-mouse layer.
- Natural scroll flips Proportional scroll's vertical direction, like the wheel. The per-slot reverse flags are separate.
- There is no burst cap and no catch-up: motion that can't be sent in the current report is dropped, never replayed later. A rate limit, when set, is the slot's visible minimum interval between presses.

## Output

Presses are paced by the host: each press waits until USB has accepted the previous report. `pointing/point_transport.c` wraps the host driver (as `keytest.c` does) so QMK's keyboard, NKRO and consumer reports pass through the output broker (`pointing/point_output.c`), which adds its own press on top and sends with `usb_endpoint_in_send()` to learn whether each report was accepted. QMK core is unchanged.

## Protocol

Advertised by GET_INFO byte 19 bit 5 (`0x20`). Commands `0x2F` GET, `0x30` SET, `0x31` STATUS and `0x32` STOP; the payloads are in [protocol.md](protocol.md#pointing-tools-experimental). `0x2E` and byte 19 bit 4 are taken by the touch gesture experiment, so these numbers were the next free ones on every branch when they were chosen.

### `speeds` (16 bytes)

A little-endian bit stream, least significant bit first:

| Bits | Field |
|---|---|
| 6 | shared horizontal speed index |
| 6 | shared vertical speed index |
| 1 | shared speeds linked (then horizontal = vertical) |
| 1 | use shared speeds for every tool |
| 28 × 4 | per slot: horizontal index (6), vertical index (6), linked (1), use shared speeds (1), rate limit in ms (8: `0` none, else `4`-`250`), reversal tolerance − 1 (6) |
| 2 | zero |

A speed index `i` (`0`-`63`) is `round(1024 × 2^(−i/9))` counts at 800 CPI per step, from 1024 down to 8; the default is `36` (64 counts). For Proportional scroll the reversal field holds the dead zone + 1 (`0`-`63` counts, default 7). SET refuses linked speeds that differ, rate limits of `1`-`3` or over `250`, and nonzero padding.

### `tools` (3 bytes)

24 bits, little-endian: per slot (slot 1 in the low bits) the action ID (3 bits) then reverse horizontal (1) and reverse vertical (1); bits 20-23 zero.

## Storage

- The speeds live in bytes 80-99 of the 100-byte VIA custom config block: `0x50`, `3`, the 16 `speeds` bytes, then a CRC-16/CCITT (initial `0xFFFF`, polynomial `0x1021`) over the first 18 bytes, little-endian. Records from earlier test builds (record versions 1 and 2) still load.
- The tools live in QMK's 32-bit `eeconfig_user` word: the 20 `tools` bits, the tag `0xA` in bits 20-23, and a CRC-8 (polynomial `0x07`, initial `0`) of the low 24 bits in bits 24-31.
- Each record is checked on its own; a bad one loads the defaults for that part only. A save reads both back and fails with status `4` if they differ. The two writes are not atomic, so a power cut during a save can mix old and new.
- The settings live on the USB half and are not copied to the other half.
- Installing a different firmware build resets every setting, these included ([settings-storage.md](settings-storage.md)).
- The touch gesture experiment stores its block at bytes 78-99 of the same VIA area, so the two experiments can't share a build until one of them moves.

## Not built yet

Activation by layer, holding a modifier through a tool (Shift+Arrows), macOS shortcut presets, copying the settings to the other half, and atomic two-record saves.
