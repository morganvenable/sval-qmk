# Scan Lab: matrix timing characterization

Svalboard reads its optical key matrix one row at a time: enable the row,
wait for the sense lines to settle (**pre-wait**), read the six columns,
release the row, wait for the lines to recover (**post-wait**), move on.
Those two waits set the scan rate, and the right values depend on the
analog front end of the board revision.

Scan Lab is a characterization mode built into the firmware so the right
values can be measured from the host, with no oscilloscope. It is driven
by the keybard-ng **Scan Lab** panel, or by anything that can send VIA
custom-value packets.

## Hardware revisions

| Revision | Strap on `SVAL_HW_REV_PIN` | Sensor LED resistors | Notes |
|----------|----------------------------|----------------------|-------|
| A        | open (reads high)          | 330 Ω everywhere      | Original boards. Scan timing from the turbo table unless explicit values are set. |
| B "flipfet" | bridged to ground (reads low) | 100 Ω side keys, 68 Ω centre keys and thumb down | Revised analog front end with isolated sense channels. Lines settle more slowly, so the board boots on explicit timing: 100 µs pre-wait, 5 µs post-wait. |

The pin is read once at boot with the internal pull-up enabled. It defaults
to `GP22` and can be moved with `-DSVAL_HW_REV_PIN=GPxx`. Prototypes without
the strap can be built with `-DSVAL_HW_REV_FORCE=1` (or `=0`). Each half
detects its own revision; the Scan Lab status reports both so a mismatch is
visible.

Key polarity reminder: centre (down) keys are **active dark**, light passes
unless the key is pressed. Side keys are **active light**, light only passes
when pressed. The pushed-state tables in `config.h` encode this; revision B
has its own thumb-row table (`MATRIX_COL_PUSHED_STATES_THUMBS_FLIPFET`) that
starts identical to revision A. The settle probe reports measured polarity,
so the table can be corrected from data.

## Measured on revision B

First characterization, one left half with a pmw3389 trackball, nothing
pressed except where noted:

| Signal | Settle after row-on | Recovery after row-off |
|--------|---------------------|------------------------|
| Centre keys (68 Ω), unpressed | 21–27 µs | ≤ 1 µs |
| Side keys (100 Ω), held | 11–15 µs | ≤ 1 µs |

No line crossed the input threshold more than once. The thumb-down key
measures active light, so the revision A thumb table is correct for
revision B. A pre-wait sweep at 5 µs steps with 10 µs post-wait was clean
from 150 µs down to 30 µs, failed at 25 µs, and read every centre key wrong
at 15 µs and below. The defaults of 100 / 5 µs therefore carry better than
3× margin on settle and 5× on recovery, and still scan faster than the
revision A default of 90 / 90 µs.

## Power: frame pacing

Sensor LED duty cycle = rows × (pre-wait + read) ÷ frame period. Without
pacing the scan runs back to back, some row is always lit, and the LEDs sit
at close to 100 % duty. Three saved values (VIA ids 20–22, Pointing Device →
Advanced) make the period explicit:

- `scan_period_us`: frame period while active. `0` = unpaced. The gate is
  non-blocking: when it is too early for the next frame the matrix reports
  "no change" and the main loop goes on servicing USB, the pointing device
  and the split link.
- Light idle: after `scan_idle_after_ms` (ms, `0` = never) without a raw
  matrix change, the period becomes `scan_idle_period_ms` (ms). Meant for a
  short wake-up, e.g. 100 ms after 2 s quiet.
- Deep idle: after `scan_deep_after_s` (seconds, `0` = never) the period
  becomes `scan_deep_period_ms` (ms, up to 65 s). Meant for long absences,
  e.g. a 1 s wake-up after 10 minutes.
- A stage whose period is at or below the active period changes nothing.
  The first raw matrix change restores the active period on the next frame,
  so the only cost is one idle-length frame on the first key after a pause.

VIA ids 21–24 carry the four idle values. Revision B defaults: 45 µs
pre-wait, 5 µs post-wait, 1000 µs period, light idle 1 ms after 1000 ms
(no slow-down), deep idle off. That is about 29 % measured duty at the USB
poll rate with no latency trade-off unless a user opts in. Revision A
defaults to unpaced, as before.

The firmware measures the real frame interval and LED-on time per frame
(smoothed) and the Scan Lab reports them (op `0x11`), so duty is read, not
assumed. Measured on a revision B left half with a pmw3389 trackball at
45 / 5 µs:

| Frame period | Measured frame | LED-on per frame | LED duty | Scan rate |
|--------------|----------------|------------------|----------|-----------|
| 0 (unpaced)  | 536 µs         | 280 µs           | 52 %     | 1866 Hz   |
| 1000 µs      | 1023 µs        | 299 µs           | 29 %     | 978 Hz    |
| 2000 µs      | 2021 µs        | 295 µs           | 15 %     | 495 Hz    |

LED-on is about 58 µs per row: the 45 µs pre-wait plus roughly 13 µs of
row switching and column reads. Unpaced, the rest of the main loop (the
trackball read, USB and debounce) already keeps duty near half; pacing
makes it a chosen number.

Total current at the USB cable for the same half, measured with a USB
ammeter:

| Frame period | LED duty | Total current | Sensor LEDs |
|--------------|----------|---------------|-------------|
| 65 ms        | 0.5 %    | 60 mA         | ~0          |
| 2 ms         | 15 %     | 80 mA         | 20 mA       |
| 1 ms         | 29 %     | 90 mA         | 30 mA       |
| unpaced      | 58 %     | 125 mA        | 65 mA       |

The relation is linear: about 110 mA while a row is lit, so sensor LED
current is 1.1 mA per percent of duty. The 60 mA floor is the MCU, the
trackball and the two RGB LEDs, and is the larger share at the 1 ms
default. The keybard-ng Scan Lab shows an estimated total next to the
measured duty using this model; re-measure the floor at a 65 ms period
on other variants. The master pushes all three values to the other half with the
timing values. Scan Lab sweeps run unpaced so they finish quickly.

## Idle power features

Frame pacing only governs the sensor LEDs. The rest of the board's draw
is the trackball sensor, the MCU and the RGB, and each has a runtime
switch in `saved_values.idle_flags` (VIA ids 25–27, synced to the other
half, reported in POWER byte 22 bits 2–4) so its share can be measured
alone from the Scan Lab panel:

- Trackball rest mode (bit 0): QMK's PMW33xx driver clears Config2,
  which disables the sensor's own rest modes. With the flag on, Config2
  is written back to its power-on default 0x20 after init and after the
  settings are read. The sensor then steps down by itself after about
  0.5 s / 10 s / 10 min without motion (run ≈ 21 mA, rest 1 ≈ 3 mA, rest
  2/3 well under 0.1 mA, datasheet typicals) and wakes on motion. In rest
  2 and 3 it looks for motion every 100 ms / 500 ms, so the first
  movement after a long pause can lag by that much.
- Dim RGB when idle (bit 1, master only): light idle divides the strip
  brightness by `SVAL_IDLE_RGB_LIGHT_DIV` (4), deep idle switches it off,
  the first key or ball motion restores it. Layer colour changes while
  dimmed store the awake brightness, so the dimmed value never reaches
  EEPROM. Because light idle now dims the RGB, the revision B default
  light-idle timeout is 10 s (its period stays 1 ms, so keys are
  unaffected).
- Sleep between scans (bit 2): when a frame period is set and the next
  frame is not due, the matrix scan naps with `chThdSleepMicroseconds`
  (at most `SVAL_SLEEP_MAX_US`, 1 ms, per nap so USB, the pointer and the
  split link stay responsive) instead of spinning, which lets ChibiOS's
  idle thread park the core in WFI. The last nap before a frame wakes
  slightly early and busy-waits the remainder so frames start on time.

Two more switches act only in deep idle:

- Low clock (bit 3) with a clock choice (`scan_deep_clock_idx`, VIA id
  30: 48 or 24 MHz from the USB PLL, or the 12 MHz crystal): clk_sys and
  clk_peri are switched and the system PLL is powered down, the sequence
  the pico-sdk uses for its 48 MHz mode. USB keeps working at all three
  because it runs from the USB PLL; the ChibiOS timer and `wait_us` run
  from the 1 MHz TIMER tick, so scan timing is unchanged. Every enabled
  PIO state machine (split serial, WS2812) has its divider rescaled on
  the switch and restored exactly afterwards, so the split baud and LED
  timing hold. The clock is only lowered once the master's RGB has made
  its deep-idle write at full speed (the WS2812 program needs 20 MHz),
  and restoring the RGB raises the clock first. The first input restores
  125 MHz on the next scan pass.
- Long naps (bit 4): deep idle naps 20 ms at a time on the master and
  4 ms on the other half (inside the 20 ms split-transaction timeout)
  instead of 1 ms. The sensor only reports every 100–500 ms in rest.

Both halves also stop clocking blocks the build never uses (ADC, PWM,
I2C, RTC, the unused SPI, the UARTs, JTAG, TBMAN) while the core is in
WFI, and ball motion or input on the other half counts as activity for
the scan idle stage, so rolling the ball wakes everything.

Measured on one revision B half with a pmw3389, deep idle at 500 ms
frames:

| State | Current |
|---|---|
| 1 ms pacing, no idle features | 90 mA |
| Sensor rest + RGB off + WFI, 125 MHz, 1 ms naps | 40 mA |
| + 48 MHz, 20 ms naps | 24 mA |
| + 12 MHz | 24 mA |
| Awake again after ball motion | 80 mA |

Going from 48 MHz to 12 MHz changed nothing, so the RP2040 is out of
the picture at this point. The 24 mA that remain are the linear
regulator and the standing current of the analog front end, which only
a hardware change can reduce (a power gate on the front end and the
sensor module, driven from the deep-idle transition, would be the
natural next step and the firmware hook is one GPIO). 48 MHz is the
default choice; 24 and 12 are there for measurement.

Op `0x12` (IDLE) reports what the features are actually doing, which is
what the panel's diagnostics line shows: the sensor's run/rest mode from
its own Motion byte, the Config2 rest bit, RGB brightness now versus
awake, and how long keys and ball have been quiet. Check it before
trusting a toggle.

## Reflashing without buttons

Builds with `SVAL_HOST_BOOTLOADER` (the `scanlab` keymap sets it; add
`-DSVAL_HOST_BOOTLOADER=1` for others) accept a two-stage reboot over the
Scan Lab channel: `REBOOT_ARM` returns a one-time token valid for 5 s, and
`REBOOT_GO` with that token acknowledges and then, 100 ms later from the
housekeeping loop, calls `reset_keyboard()`, which jumps to the RP2040 ROM
bootloader. The half enumerates as the `RPI-RP2` mass-storage drive.

`keyboards/svalboard/tools/flash.sh <image.uf2>` waits for that drive,
copies the image and waits for the reboot. It works from WSL (through
PowerShell), Linux and macOS. Afterwards reconnect from keybard-ng's
permitted-keyboard list; the WebHID permission survives a reflash.

Both halves: send the reboot to the other half first (it drops into the
bootloader and waits, dark, on the link), then to the connected half; flash
the connected half; move the USB cable to the other half, which enumerates
immediately; flash it; reconnect. No buttons.

The command is off by default in production keymaps because any web origin
that has been granted HID access could otherwise put a keyboard into
bootloader mode.

## Scan timing settings

Two saved values, exposed in the Pointing Device → Advanced menu and as VIA
custom values 13 and 14 on the keyboard channel:

- `scan_prewait_us`: row-on settle time before the columns are read.
- `scan_postwait_us`: row-off recovery time after the columns are read.

`0` means "use the turbo-scan table entry" (the existing Scan Speed
setting). Any non-zero value overrides the table for that wait. The master
pushes both values to the other half every 500 ms, so setting them on the
connected half configures the whole keyboard. VIA value 15 reads back the
hardware revision.

## Instruments

### Settle probe

For one row on one hand: release every row and wait 1 ms, record the idle
level of each column, enable the row and sample all columns in a tight loop
for 1.5 ms with microsecond timestamps, then release the row and sample for
another 1.5 ms. Interrupts are off for the sample windows so the timestamps
are clean.

Per column you get:

- **settle**: time after row-on of the last level change (0 if the line
  never moved);
- **recovery**: time after row-off of the last level change;
- the number of level changes in each phase (more than one means the line
  wandered across the input threshold before settling);
- the idle, lit and released levels.

With nothing pressed, a column that changes when its row lights is an
active-dark key. That is the polarity check. The settle figure for those
keys is the row-on wait the hardware needs; the recovery figure is the
row-off wait. Side keys contribute the same measurement when they are held.

### Validation sweep

For one hand: capture a reference frame at a safe timing (500/500 µs; eight
identical consecutive frames are required, otherwise the sweep reports
`REF_FAIL`), then scan N frames at the candidate pre/post-wait and count,
per key, how many frames disagreed with the reference. Key events are
suppressed for the duration, so the host sees no keystrokes. Sweeping the
candidate timing downward and watching where mismatches appear gives the
end-to-end limit including P-FET switching, firmware overhead and whatever
keys are being held.

A typical session, which the panel automates:

1. Probe every row on both hands with nothing pressed. Read off the
   slowest settle and recovery; note any key whose polarity disagrees with
   the pushed-state table.
2. Sweep pre-wait from 300 µs down to 5 µs at a fixed generous post-wait,
   200 frames per step, nothing pressed. Then repeat holding a few side
   keys and a centre key.
3. Sweep post-wait the same way at the chosen pre-wait.
4. Apply the lowest clean values plus margin as the explicit timing.

## Wire format

All packets are VIA custom-value packets on channel `0x53`:
`[command][channel][value_id][value_data...]`. The response is written over
`value_data` (up to 23 bytes). Hands are addressed as 0 = left, 1 = right;
the master executes requests for its own hand and relays the others over
split RPC (`KEYBOARD_SYNC_B`). A first response byte of `0xFF` means the
other half did not answer.

Set commands (`0x07`), `value_data = [hand][args...]`:

| value_id | op | args |
|----------|----|------|
| `0x01` | SET_MODE | `mode` (0 off, 1 sweep), `prewait` u16, `postwait` u16, `frames` u16 |
| `0x02` | PROBE | `row` (0 = thumbs, 1–4 = fingers) |
| `0x03` | ABORT | |
| `0x04` | REBOOT_ARM | response `[0..1]` token, `[2]` 1 if supported |
| `0x05` | REBOOT_GO | `token` u16; response `[0]` 1 if accepted, `[2]` 1 if supported |

Get commands (`0x08`), `value_id = op | hand << 3 | row`:

| op | response |
|----|----------|
| `0x12` IDLE | `[0]` idle flags, `[1]` sensor present, `[2]` sensor mode (bits 0–1: 0 run, 1–3 rest 1–3; bit 7 valid; bit 6 lifted), `[3]` Config2 as last written, `[4]` RGB value now, `[5]` RGB value awake, `[6]` RGB stage (0 awake, 1 dimmed, 2 off), `[7]` RGB enabled, `[8]` scan idle stage, `[9..12]` ms since any input, `[13..16]` ms since a matrix change, `[17..20]` ms since pointer motion, `[21]` system clock now (MHz), `[22]` configured deep-idle clock (MHz) |
| `0x11` POWER | `[0..1]` saved period (µs), `[2..3]` light idle period (ms), `[4..5]` light idle timeout (ms), `[6..7]` measured frame interval (µs, capped), `[8..9]` measured LED-on per frame (µs), `[10]` stage (0 active, 1 light idle, 2 deep idle), `[11..12]` effective period (µs, capped), `[13..14]` effective pre-wait, `[15..16]` effective post-wait, `[17]` rows, `[18..19]` deep idle timeout (s), `[20..21]` deep idle period (ms), `[22]` bit 0 host bootloader supported, bit 1 reboot armed, bit 2 pointer rest, bit 3 RGB dim, bit 4 CPU sleep, bit 5 low clock, bit 6 long naps |
| `0x10` STATUS | `[0]` proto version, `[1]` hw revision, `[2]` sweep state (0 idle, 1 capturing reference, 2 running, 3 done, 4 reference failed), `[3..4]` frames done, `[5..6]` frames target, `[7]` reference valid, `[8..9]` effective pre-wait, `[10..11]` effective post-wait, `[12]` this half is left, `[13]` finger pushed-state mask, `[14]` thumb pushed-state mask, `[15]` probe valid, `[16]` probed row, `[17..18]` saved pre-wait, `[19..20]` saved post-wait, `[21]` turbo index, `[22]` other half connected |
| `0x20` SWEEP_ROW | `[0..11]` six u16 mismatch counts, `[12]` reference row bits, `[13]` last raw row bits, `[14]` sweep state, `[15]` reference valid |
| `0x40` PROBE_ON | `[0..11]` six u16 settle times (µs), `[12..17]` six change counts, `[18]` idle level mask, `[19]` lit level mask, `[20]` valid |
| `0x60` PROBE_OFF | `[0..11]` six u16 recovery times (µs), `[12..17]` six change counts, `[18]` released level mask, `[19]` lit level mask, `[20]` valid |

Row results are only returned for the row that was last probed on that
hand; other rows answer all zeros with `valid = 0`.

All multi-byte values are little-endian. Column order follows
`MATRIX_COL_PINS`; for finger rows the columns are S, E, D, N, W, (unused),
for the thumb row OL, OU, D, IL, MODE, DOUBLE.
