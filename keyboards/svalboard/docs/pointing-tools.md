# Pointing tools (experimental)

While a tool key is held or toggled, the pointing modules stop moving the cursor and drive a tool instead, on either half: each step of travel sends one volume, arrow, tab, task, zoom or undo/redo press, or the displacement from where the key went down sets a scroll speed. The board sends standard keyboard, consumer and mouse reports only, so no host software is involved.

The tools are built only with `SVAL_POINT_CONTROLS=yes` and are not in released firmware:

```
make svalboard/left:sval SVAL_POINT_CONTROLS=yes
make svalboard/right:sval SVAL_POINT_CONTROLS=yes
```

These are the normal auto-detecting images: one per side, with every pointing driver built in. This page describes what is built (settings version 5).

## Tools

Four tool slots. Each slot runs one of eight actions:

| ID | Action | Axis | Windows | macOS |
|---|---|---|---|---|
| 0 | Volume | vertical | consumer Volume Up / Down | the same |
| 1 | Arrows | both | Right, Left, Up, Down, with the held modifiers | the same |
| 2 | Tabs | horizontal | Ctrl+Tab / Ctrl+Shift+Tab | Command+Shift+] / Command+Shift+[ |
| 3 | Tasks | horizontal | Alt+Tab / Alt+Shift+Tab, Alt held until the session ends | Command+Tab / Command+Shift+Tab, Command held |
| 4 | Zoom | vertical | Ctrl+= / Ctrl+- | Command+= / Command+- |
| 5 | Undo / Redo | horizontal | Ctrl+Z / Ctrl+Shift+Z | Command+Z / Command+Shift+Z |
| 6 | Undo / Redo (Ctrl+Y) | horizontal | Ctrl+Z / Ctrl+Y | Command+Z / Command+Shift+Z |
| 7 | Proportional scroll | both | wheel speed proportional to the displacement from where the tool started | the same |

The defaults are slot 1 Volume, 2 Arrows, 3 Tabs and 4 Tasks. Shortcuts assume a US layout. The board switches to the macOS column by itself when it detects a macOS or iOS host (the same detection that turns the axis lock off there); there is no setting. On macOS both Undo / Redo variants redo with Command+Shift+Z, the Mac standard, because many Mac apps ignore Command+Y.

The slots are shared by every pointer. Each slot has a horizontal and a vertical speed (travel per step), an optional rate limit, a reversal tolerance (for Proportional scroll, the center dead zone) and a reverse flag per axis. "Use shared speeds for every tool" is on by default; with it off, a slot can follow the shared speeds or set its own.

### Every action has its own step and pace

One step of a tool is a different size of thing for each action: a notch of volume, a character, a tab, a window, a zoom level, an undo. So every action has its own default travel per step and its own default pace, whatever slot runs it. The travel is a multiple of the slot's speed (64 counts at 800 CPI, about 2 mm of ball travel, at the default speed); the pace is the shortest time between presses while the slot's rate limit is 0.

| Action | Travel per step (default speed) | Default pace | Why |
| --- | --- | --- | --- |
| Volume | 1x: 64 counts, about 2 mm | none | Steps are small and the host shows the level at once, so fine, fast steps feel like a knob. |
| Arrows | 2.5x: 160 counts, about 5 mm | 65 ms, about 15 a second | One character or line per step: at 1x a small roll jumps several, so a step is a deliberate nudge, and the pace stays near key repeat so long moves are still quick. |
| Tabs | 5x: 320 counts, about 10 mm | 140 ms, about 7 a second | A whole tab per step, and the browser needs time to show each one: a deliberate roll moves one or two, and you can stop on the one you want. |
| Tasks | 5x: 320 counts, about 10 mm | 140 ms, about 7 a second | The same for windows: the switcher needs time to move its highlight. |
| Zoom | 3x: 192 counts, about 6 mm | 100 ms, about 10 a second | Each step redraws the page at a new size; a little more travel and a pace the redraw can follow keep it from overshooting. |
| Undo / Redo (both) | 4x: 256 counts, about 8 mm | 150 ms, about 7 a second | Each step changes the document and is easy to lose track of, so steps are big and slow enough to watch each one land. |
| Proportional scroll | unchanged | none | Continuous scrolling, not steps: its speed and rate limit work as they always have. |

The slot's settings still apply on top: its speed scales the action's travel (half the speed, half the step), and a rate limit set for the slot replaces the default pace, faster or slower.

### Held modifiers

- **Arrows** goes out with the modifiers the user holds: Shift+arrow selects, Ctrl+arrow jumps by word, and so on. It waits while any other key is held.
- **Tabs, Tasks, Zoom and both Undo / Redo** wait while any key or modifier is held, because an extra modifier changes what their shortcut means (Ctrl+Shift+Z is redo, not undo).
- **Volume** also waits while any key or modifier is held, and while a consumer key is.
- **Proportional scroll** is never held back.

## Keys

The whole `QK_KB` block, `0x7E00`-`0x7E3F` (64 codes), is reserved for the pointing tools. The codes are permanent because layouts store them: new keys go after the last one, and no code is renumbered or reused. A code in the block with no key yet does nothing.

Tools aren't left or right. While a tool key is held or toggled on, every pointing module runs that tool, so whichever pointer moves runs it, on either half, and both at once if both move. Each pointer keeps its own motion, steps and Proportional scroll center. The slot settings are one shared set: four slots, not four per pointer.

Each tool key is `0x7E00 + slot * 2 + mode`: slot `0`-`3`; mode `0` hold, `1` toggle. The name gives the slot, not its action, which is a setting of the slot.

| Code | Name | Meaning |
|---|---|---|
| `0x7E00` | `SV_POINT_TOOL1_HOLD` | slot 1, hold |
| `0x7E01` | `SV_POINT_TOOL1_TOGGLE` | slot 1, toggle |
| `0x7E02` | `SV_POINT_TOOL2_HOLD` | slot 2, hold |
| `0x7E03` | `SV_POINT_TOOL2_TOGGLE` | slot 2, toggle |
| `0x7E04` | `SV_POINT_TOOL3_HOLD` | slot 3, hold |
| `0x7E05` | `SV_POINT_TOOL3_TOGGLE` | slot 3, toggle |
| `0x7E06` | `SV_POINT_TOOL4_HOLD` | slot 4, hold |
| `0x7E07` | `SV_POINT_TOOL4_TOGGLE` | slot 4, toggle |
| `0x7E08`-`0x7E0F` | | retired (the right pointer's slot keys in early test builds); do nothing |
| `0x7E10` | `SV_POINT_STOP_ALL` | stop every tool; an open task switcher is canceled with Escape |
| `0x7E11` | `SV_POINT_TRACE` | print counters to the console (diagnostic; Keybard doesn't offer it) |
| `0x7E12`-`0x7E13` | | retired (Finer and Coarser in early test builds); do nothing |
| `0x7E14`-`0x7E3F` | | reserved for later pointing tools keys |

Retired codes are never reused, so a layout that still holds one gets a key that does nothing rather than a different tool. Travel per step is set only with the tool speeds; no key changes it.

Earlier test builds had a set of slot keys per pointer (`SV_POINT_L_TOOL1_HOLD` at `0x7E00`, `SV_POINT_R_TOOL1_HOLD` at `0x7E08`, ...), named them after each slot's default action before that (`SV_POINT_L_VOLUME_HOLD`, then `ARROWS`, `TABS` and `TASKS`), and called `0x7E10` `SV_POINT_CANCEL`. Keybard reads all of those names from layout files as the matching `SV_POINT_TOOL<n>` key (and `SV_POINT_STOP_ALL`); Finer and Coarser load as no key, with a warning. The left-pointer codes `0x7E00`-`0x7E07` keep their meaning, now for every pointer.

- The newest held key wins. Holding a second tool key replaces the first; releasing the newest stops it, and the tools stay off until a tool key is pressed again: an older key still held doesn't come back. Tools don't stack.
- A held key wins over the toggle, and releasing it returns to the toggle if one is on. There is one toggle for the whole board: toggling another slot replaces it, toggling the same slot clears it. Every pointer follows the same keys.
- A release is matched by the key's position, so changing layers while a key is held still releases it.
- Only physical keys activate tools. Codes from combos, macros, tap dances and other synthetic events are dropped; mod-tap and layer-tap wrapping are not supported.
- No layer turns the pointers into a tool by itself; put a tool key on the layer instead.
- Changing the tool ends what each pointer was sending and starts the new tool from rest. Pressing a Proportional scroll key re-centers it, even when that tool was already on.
- While a Tasks session is open: Enter or a left click accepts, Escape or another click cancels, any other key accepts and then goes through. A held Tasks key never times out: the switcher stays open as long as the key is held, and releasing it accepts. A toggled session is cancelled after 10 s without a step, because nothing on the board is held to show that Alt (Command) is still down on the host; that is the only timed end of a tool. The session belongs to the pointer whose step opened it; the other pointer is ignored until it ends. After it ends, a held Tasks key does nothing more until it is released.

## Motion

- Counts are normalized to 800 CPI before the speeds apply, so a step takes the same travel at any CPI. Trackballs and the trackpad use the CPI set for that hand (the trackpad caps it at its own resolution). A TrackPoint ignores CPI and reports force rather than distance, so its counts are used as they come, as a logical 800 CPI. Each hand's module comes from the same detection `POINTING_HARDWARE` reports; a hand with no module simply produces no steps.
- Sniper and boost don't touch a pointer running a tool: the same travel per step, the same Proportional scroll speed, and the same motion into the axis lock. A tool's feel comes only from its slot settings. With no tool on, both pointers get sniper and boost as usual.
- Every tool obeys the board's one free-scroll / axis-lock policy (`pointing/scroll_policy.c`), shared with wheel scrolling on both pointers. It applies while axis lock is on and the host isn't macOS.
- While a tool is on, neither pointer moves the cursor, and the tool overrides drag-scroll; their motion still feeds the shared axis lock. Tool motion never turns on the auto-mouse layer.
- Natural scroll flips Proportional scroll's vertical direction, like the wheel. The per-slot reverse flags are separate.
- There is no catch-up. Motion is turned into steps once per scroll batch (10 ms), and the steps one batch can't send, because of the rate limit or because USB hasn't taken the previous press, are dropped at the end of that batch, never queued. When the ball stops, the steps stop with the batch it stopped in; a hard flick gives only the presses the rate limit allows while the ball is moving. The rate limit is the board's: two pointers running the same tool share it.

## Output

Presses are paced by the host: each press waits until USB has accepted the previous report. `pointing/point_transport.c` wraps the host driver (as `keytest.c` does) so QMK's keyboard, NKRO and consumer reports pass through the output broker (`pointing/point_output.c`), which adds its own press on top and sends with `usb_endpoint_in_send()` to learn whether each report was accepted. QMK core is unchanged.

## Protocol

Advertised by GET_INFO byte 19 bit 7 (`0x80`). Commands `0x34` GET, `0x35` SET, `0x36` STATUS and `0x37` STOP; the payloads are in [protocol.md](protocol.md#pointing-tools-experimental). Upstream and the open branches own `0x2E` (touch gestures), `0x2F` (`POINTING_CONTACTS`), `0x33` (the one-image side command) and byte 19 bits 4-6, so these were the next free numbers on every branch on 2026-10-25. Builds before then used `0x2F`-`0x32` and bit 5.

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

A speed index `i` (`0`-`63`) is `round(1024 × 2^(−i/9))` counts at 800 CPI per step, from 1024 down to 8; the default is `36` (64 counts). Each action multiplies it by its own travel (see [Every action has its own step and pace](#every-action-has-its-own-step-and-pace)). A rate limit of `0` means the action's default pace. For Proportional scroll the reversal field holds the dead zone + 1 (`0`-`63` counts, default 7). SET refuses linked speeds that differ, rate limits of `1`-`3` or over `250`, and nonzero padding.

### `tools` (3 bytes)

24 bits, little-endian: per slot (slot 1 in the low bits) the action ID (3 bits) then reverse horizontal (1) and reverse vertical (1); bits 20-23 zero.

## Storage

The VIA custom config block is 144 bytes, and each user has its own range ([via_custom_map.h](../via_custom_map.h)) whether or not it is built in:

| Bytes | User |
|---|---|
| 0-99 | board settings (`saved_values_t`) and their magic |
| 100-119 | pointing tools speed record |
| 120-143 | touch gesture experiment (a 22-byte block, two bytes spare) |

The header refuses to build if two ranges overlap, the firmware checks at build time that each record fits its range, and `tests/sval_pointing` checks the map and that a save writes only bytes 100-119.

- The speed record, bytes 100-119: `0x50`, `3`, the 16 `speeds` bytes, then a CRC-16/CCITT (initial `0xFFFF`, polynomial `0x1021`) over the first 18 bytes, little-endian.
- The tools live in QMK's 32-bit `eeconfig_user` word: the 20 `tools` bits, the tag `0xA` in bits 20-23, and a CRC-8 (polynomial `0x07`, initial `0`) of the low 24 bits in bits 24-31.
- Each record is checked on its own; a bad one loads the defaults for that part only. A save reads both back and fails with status `4` if they differ. The two writes are not atomic, so a power cut during a save can mix old and new.
- The settings live on the USB half and are not copied to the other half.
- Installing a different firmware build resets every setting, these included ([settings-storage.md](settings-storage.md)). The block's size is part of the VIA layout stamp, so even a dev `keep` build resets the keymap and macros once when it first replaces a build with the old 100-byte block. Records the earliest test builds left at bytes 80-99 are not read.

## Not built yet

Activation by layer, combos as tool keys, copying the settings to the other half, and atomic two-record saves.
