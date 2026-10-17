# The Sval protocol

Svalboard QMK gives desktop apps a two-way connection to the keyboard over Raw HID, alongside its normal keyboard and mouse interfaces. Keybard uses it through the browser's WebHID; native apps can use any HID library. The keyboard runs its saved layout on its own, so no app has to stay open.

## What an app can do

**Read:** the board definition and geometry, every layer's key assignments, tap dances, combos, overrides, macros and the other behaviors, settings, active and default layers, and which physical keys are held.

**Write:** key assignments, behaviors, macros, pointing and other settings, and the active layer. Selecting a layer changes runtime state only; it doesn't rewrite the stored layout.

Host apps can read and control layer state.

## Sharing the keyboard between apps

Every message is one 64-byte report (it was 32 bytes before this release; apps take the size from the HID descriptor):

| `0xDD` | client ID (4 bytes) | `0xDF` Sval or `0xFE` VIA | payload |
| --- | --- | --- | --- |

An app asks for a client ID by sending a random 20-byte nonce with ID `0`. The keyboard echoes the nonce with a new ID and a lease of about two minutes. The app sends that ID with each request, the keyboard echoes it in the reply, and each app keeps only its own replies. Before the lease ends, the app asks for a new ID. The keyboard stores nothing per app, so a crashed app's ID simply expires.

Client IDs keep replies apart; they don't lock settings. Two apps editing the same thing can overwrite each other, so use one editor and let other tools read.

## Limits

- Apps poll for state; there is no push stream of key events, so a very short tap can fall between two polls.
- There is no notification when the layout changes. An app that caches the layout, such as a trainer, should reload after you edit.
- Whether two apps can open the keyboard at once depends on the operating system.

## Version 3

The launch firmware speaks Sval protocol version 3:
- 16-bit indices reach all 256 entries of each behavior table.
- Sparse reads skip empty entries.
- 32-bit macro offsets reach past 64 KiB.
- 32-bit layer masks report active and default layers.
- `GET_INFO` reports the protocol version, feature flags and the QMK keycode numbering the board uses.
- `GET_INFO` byte 18 reports storage events: bit 0, settings were reset because storage could not be read; bit 1, a change could not be saved since this boot; bit 2, settings were reset to the build's defaults because a different firmware build was installed. Bits 0 and 2 stay set until the host sends `0x2A`. Older firmware never sets bit 2.

Check feature flags before using an extension, send one request at a time per connection, and check the client ID and command in every reply.

## Pointing hardware

`POINTING_HARDWARE` (`0x2C`) tells a host which side each half thinks it is, which pointing module its firmware drives, and which module actually answered. Released firmware drives whichever module is attached: one image per side, which probes for the module at startup (flags bit 3). The per-module builds (`svalboard/trackball/pmw3360`, `pmw3389`, `trackpoint`, `azoteq`) still build but are not released; on one of those the wrong module stays dead with no other sign, so a host can use this report to tell the user. On an auto build, `built_for` `0` means no module has answered yet. The half looks again during its first 30 s (below), so a module that comes up or is plugged in by then starts on its own and the report changes; one plugged in later starts after a replug.

- Discovery: GET_INFO byte 19 (the second feature byte), bit 1 (`sval_flag2_pointing_hardware`). Older firmware leaves it zero.
- Request: `DF 2C`
- Response: `DF 2C <format> <this half: 5 bytes> <other half: 5 bytes>`. `format` is `1`; `0` means the board has nothing to report. "This half" is the one plugged into USB.
- Each half: `<flags> <built_for> <detected> <Product_ID> <Inverse_Product_ID>`.
  - `flags`: bit 0 the block holds a report, bit 1 the half is the left half, bit 2 the sensor driver initialized, bit 3 the firmware drives whichever module is attached (the plain `left`/`right` builds), so `built_for` is the module it chose after the probe, or `0` when none answered.
  - `built_for`: `0` no pointing module, `1` PMW3360 trackball, `2` PMW3389 trackball, `5` Azoteq trackpad, `6` TrackPoint.
  - `detected`: `0` not checked yet, `1` PMW3360 (`42 BD`), `2` PMW3389 (`47 B8`), `3` no module answered, `4` the trackball bus answered with some other ID, `5` Azoteq (IDs: product number, high byte first), `6` TrackPoint (first ID byte: its PS/2 device ID).
  - Each build checks its own module at startup. When that module doesn't answer, or the build has none, the half checks the other modules' buses about 1.5 s after boot (PS/2, then I2C, then SPI), so `detected` names whatever is attached. Until then it reads `0` (no module build) or `3` (its own module missing). While nothing works, the half looks again, its own module first, then the other buses: 3, 6, 12 and 30 s after boot, and never after that, so a board with no pointing module isn't probed again. It stops sooner once a module works, once a per-module build finds a module it can't drive, or once a module that answers has failed to start four times. A half whose module works is never probed. So `detected`, `built_for` and bit 2 can change during a half's first 30 s, after a host has connected (the other half's block follows within a presence ping, 0.5 s); a host that shows `3` or `0` should read again until then.
- The other half reports through the split link's half-second presence ping, so its block is zero while it is disconnected, for up to half a second after it connects, and when it runs firmware without this command.

## Pointing fragments

The keyboard definition (`sval.json`, keymaps `sval` and `blank`) describes each hand's pointing module as a zero-key fragment, so a host can draw it in the layout. Fragments `pointing_none` (4), `pointing_trackball_pmw3360` (5), `pointing_trackball_pmw3389` (6), `pointing_touchpad_azoteq` (7), `pointing_trackpoint` (8) and `pointing_unknown` (9) have `"kle": []` and a `widget` object (`kind`, `module`, `sensor`, `w`, `h` in key units). Instances `left_pointing` (10) and `right_pointing` (11) have `hardware_detect: true`; their selection bytes are not used.

`FRAGMENT_GET_HARDWARE` (`0x18`) reports them from the same identity `POINTING_HARDWARE` returns, for whichever half is on that hand (the other half's only while it is connected): `detected` 1 → 5, 2 → 6, 3 → 4, 4 → 9, 5 → 7, 6 → 8. It reads `0xFF` while that hand isn't known yet: no half on that side, `detected` still `0`, or `3` before the boot probe of the other buses has run (for the other half, until 2.5 s after the USB half booted). A host can re-read `0x18` until both bytes settle. A hand that reads `4` (no module) can still change until that half has run for 30 s, if its search finds one.

## Pointing motion

`POINTING_MOTION` (`0x2D`) returns each hand's pointing motion since the previous read and the pointing modes in force, for a live picture of the modules. Reading clears the counts.

- Discovery: GET_INFO byte 19, bit 3 (`sval_flag2_pointing_motion`, `0x08`). Older firmware leaves it zero.
- Request: `DF 2D`
- Response: `DF 2D` then 24 bytes (offsets from the first of them). `format` `0` means no data.

| off | size | field |
|---|---|---|
| 0 | 1 | format = `1` |
| 1 | 1 | modes: bit 0 sniper (precision) active, bit 1 boost active, bit 2 auto-mouse layer on, bit 3 axis scroll lock in effect (the setting is on and the host isn't macOS), bit 4 natural scroll, bit 5 a scroll hold or scroll toggle key is active |
| 2 | 1 | ms since the previous read, up to 255; `0` on the first read after boot |
| 3 | 10 | left hand |
| 13 | 10 | right hand |
| 23 | 1 | axis lock: `0` free, `1` locked vertical, `2` locked horizontal (`0` while axis lock is off) |

Each hand:

| off | size | field |
|---|---|---|
| 0 | 1 | flags: bit 0 valid (that hand's driver started and its reports reach the USB half), bit 1 its motion is turned into scrolling, bit 2 a value below was clamped since the last read, bit 3 auto-mouse is enabled for this hand, bit 4 touch position valid, bits 5-6 finger count (0 unknown), bit 7 zero |
| 1 | 2 | dx, int16 little-endian (+right) |
| 3 | 2 | dy, int16 little-endian (+down) |
| 5 | 1 | h, int8 (wheel, +right) |
| 6 | 1 | v, int8 (wheel, +up) |
| 7 | 1 | buttons pressed since the last read (OR) |
| 8 | 1 | touch x, 0-255 across the pad (only with bit 4) |
| 9 | 1 | touch y, 0-255 down the pad (only with bit 4) |

- The counts are both halves' reports in the native split pointing path (`pointing_device_task_combined_kb`), after rotation and invert and before sniper, boost and scroll conversion, so they are raw sensor counts at the hand's CPI. Scroll-mode motion still shows in dx/dy with bit 1 set; h and v carry what the module itself reports as wheel (an Azoteq scroll gesture).
- Motion is counted only while a host app has read `POINTING_MOTION` within the last 1.5 s (`SVAL_HOST_ACTIVE_MS`). These reads don't count as host activity, so a host that keeps polling doesn't hold the board out of deep idle; a read can then take up to one deep-idle nap (20 ms) to answer.
- A hand that is not valid (no module, the driver didn't start, or the other half is disconnected) has an all-zero block.
- Touch positions and finger counts are not reported yet: bit 4 and bits 5-6 read `0`.

Packet formats: [client-ID protocol](../../../modules/svalboard/core/docs/CLIENT_ID_PROTOCOL.md), [layer state](../../../modules/svalboard/core/docs/LAYER_STATE_PROTOCOL.md), [command definitions](../../../modules/svalboard/core/sval.h).
