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

Check feature flags before using an extension, send one request at a time per connection, and check the client ID and command in every reply.

## Pointing hardware

`POINTING_HARDWARE` (`0x2C`) tells a host which side each half thinks it is, which pointing module its firmware drives, and which module actually answered. Each module needs its own firmware (and PMW3360 and PMW3389 trackballs need different firmware), and the wrong one leaves the module dead with no other sign, so a host can use this to name the right `.uf2`.

- Discovery: GET_INFO byte 19 (the second feature byte), bit 1 (`sval_flag2_pointing_hardware`). Older firmware leaves it zero.
- Request: `DF 2C`
- Response: `DF 2C <format> <this half: 5 bytes> <other half: 5 bytes>`. `format` is `1`; `0` means the board has nothing to report. "This half" is the one plugged into USB.
- Each half: `<flags> <built_for> <detected> <Product_ID> <Inverse_Product_ID>`.
  - `flags`: bit 0 the block holds a report, bit 1 the half is the left half, bit 2 the sensor driver initialized, bit 3 the firmware drives whichever module is attached (the plain `left`/`right` builds), so `built_for` is the module it chose after the probe, or `0` when none answered.
  - `built_for`: `0` no pointing module, `1` PMW3360 trackball, `2` PMW3389 trackball, `5` Azoteq trackpad, `6` TrackPoint.
  - `detected`: `0` not checked yet, `1` PMW3360 (`42 BD`), `2` PMW3389 (`47 B8`), `3` no module answered, `4` the trackball bus answered with some other ID, `5` Azoteq (IDs: product number, high byte first), `6` TrackPoint (first ID byte: its PS/2 device ID).
  - Each build checks its own module at startup. When that module doesn't answer, or the build has none, the half checks the other modules' buses once, about 1.5 s after boot (PS/2, then I2C, then SPI), so `detected` names whatever is attached. Until then it reads `0` (no module build) or `3` (its own module missing).
- The other half reports through the split link's half-second presence ping, so its block is zero while it is disconnected, for up to half a second after it connects, and when it runs firmware without this command.

## Pointing fragments

The keyboard definition (`sval.json`, keymaps `sval` and `blank`) describes each hand's pointing module as a zero-key fragment, so a host can draw it in the layout. Fragments `pointing_none` (4), `pointing_trackball_pmw3360` (5), `pointing_trackball_pmw3389` (6), `pointing_touchpad_azoteq` (7), `pointing_trackpoint` (8) and `pointing_unknown` (9) have `"kle": []` and a `widget` object (`kind`, `module`, `sensor`, `w`, `h` in key units). Instances `left_pointing` (10) and `right_pointing` (11) have `hardware_detect: true`; their selection bytes are not used.

`FRAGMENT_GET_HARDWARE` (`0x18`) reports them from the same identity `POINTING_HARDWARE` returns, for whichever half is on that hand (the other half's only while it is connected): `detected` 1 → 5, 2 → 6, 3 → 4, 4 → 9, 5 → 7, 6 → 8. It reads `0xFF` while that hand isn't known yet: no half on that side, `detected` still `0`, or `3` before the boot probe of the other buses has run (for the other half, until 2.5 s after the USB half booted). A host can re-read `0x18` until both bytes settle.

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

## Touch gestures (experimental)

`TOUCH_GESTURES` (`0x2E`) reads and sets who owns each touch gesture on a unified Azoteq pair, and what the firmware's own gestures type. Only experimental builds have it (`SVAL_AZOTEQ_UNIFIED=yes`, or the keyless `SVAL_TOUCH_TEST_BOARD` fixtures); see `tools/multitouch/README.md`.

- Discovery: GET_INFO byte 19, bit 4 (`sval_flag2_touch_gestures`, `0x10`). Other builds answer every sub-command with status `3`.
- Values are little-endian. Status: `0` ok, `1` bad version or length, `2` a value is out of range (nothing applied), `3` this build has no touch support.

| Sub | Request after `DF 2E` | Reply after `DF 2E` |
|---|---|---|
| 0 GET | `00` | `00 status ver=1 len=21 payload[21]` |
| 1 SET | `01 ver=1 len=21 payload[21]` | `01 status`. Applies at once; saved only if something changed |
| 2 DEFAULTS | `02` | `02 status payload[21]`. Restores and saves the defaults |
| 3 STATUS | `03` | `03 status mode claim last count:u16 claims:u16 contacts_left contacts_right` |

STATUS: `mode` `0` normal, `2` unified (a host holds the pair-capture lease), plus `0x80` while chip output stays off after unified mode until both pads are all-up. `claim` `1` while a firmware gesture owns the pads. `last` the last output (`0` none, then 1-6 in the keycode order below). `count` outputs fired and `claims` gestures claimed, both since the last STATUS (read-and-clear). Contacts per hand, `0xFF` unknown.

Payload v1 (21 bytes):

| Byte | Field | Default |
|---|---|---|
| 0 | Bit 0 `scroll_owner` (0 Windows, 1 chip). Bit 1 `swipe_owner` (0 Windows, 1 firmware). Bit 2 `pinch_owner` (0 Windows, 1 firmware). Bit 3 firmware gestures may span both pads. Bit 4 the host always merges cross-pad contacts (0: only touchdowns within 150 ms). Bits 5-7 zero | `0x08` |
| 1 | Low nibble `swipe_fingers` (3-5), high nibble `sustain_min` (1-5, at most `swipe_fingers`) | `0x13` |
| 2 | `swipe_mm` (4-40) | 12 |
| 3 | `swipe_ratio` x10 (10-50) | 20 |
| 4 | `settle_ms` (0-200) | 40 |
| 5 | `swipe_window` in 10 ms (10-200) | 60 |
| 6 | `swipe_repeat_mm` (0 fire once, else 4-40) | 0 |
| 7 | `pinch_start_mm` x2 (2-40) | 8 |
| 8 | `pinch_step_mm` x2 (1-40) | 6 |
| 9-20 | Keycodes, u16 each: swipe left, right, up, down, pinch out, pinch in. `0` does nothing | `094F 0950 082B 0807 01D9 01DA` |

Keycodes are QMK keycodes: basic and modified keys are tapped, wheel keycodes (with or without modifiers) put one full detent into the pointing report with the modifiers held, and anything else (macros, layer keys) runs as a key press and release. The settings live in the VIA custom area right after the saved-values magic (22 bytes: stamp `0xA1`, then the payload); a bad stamp loads the defaults.

The contact stream (`0xFC`, not a Sval command) carries two related signals:
- Frame flag `0x40`: the firmware claimed the gesture. It is set on both pads' frames until every finger on both pads is up. A host must withdraw those contacts from the operating system at once (without a release that could read as a tap) and send nothing until all-up.
- Info-reply ownership bits: `0x10` scroll owner is the chip, `0x20` the firmware owns swipe or pinch. A host re-reads GET when they change.

Packet formats: [client-ID protocol](../../../modules/svalboard/core/docs/CLIENT_ID_PROTOCOL.md), [layer state](../../../modules/svalboard/core/docs/LAYER_STATE_PROTOCOL.md), [command definitions](../../../modules/svalboard/core/sval.h).
