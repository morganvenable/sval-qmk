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
| B "flipfet" | bridged to ground (reads low) | 100 Ω side keys, 68 Ω centre keys and thumb down | Revised analog front end with isolated sense channels. Lines settle more slowly, so the board boots on explicit conservative timing until characterized. |

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

Get commands (`0x08`), `value_id = op | hand << 3 | row`:

| op | response |
|----|----------|
| `0x10` STATUS | `[0]` proto version, `[1]` hw revision, `[2]` sweep state (0 idle, 1 capturing reference, 2 running, 3 done, 4 reference failed), `[3..4]` frames done, `[5..6]` frames target, `[7]` reference valid, `[8..9]` effective pre-wait, `[10..11]` effective post-wait, `[12]` this half is left, `[13]` finger pushed-state mask, `[14]` thumb pushed-state mask, `[15]` probe valid, `[16]` probed row, `[17..18]` saved pre-wait, `[19..20]` saved post-wait, `[21]` turbo index, `[22]` other half connected |
| `0x20` SWEEP_ROW | `[0..11]` six u16 mismatch counts, `[12]` reference row bits, `[13]` last raw row bits, `[14]` sweep state, `[15]` reference valid |
| `0x40` PROBE_ON | `[0..11]` six u16 settle times (µs), `[12..17]` six change counts, `[18]` idle level mask, `[19]` lit level mask, `[20]` valid |
| `0x60` PROBE_OFF | `[0..11]` six u16 recovery times (µs), `[12..17]` six change counts, `[18]` released level mask, `[19]` lit level mask, `[20]` valid |

Row results are only returned for the row that was last probed on that
hand; other rows answer all zeros with `valid = 0`.

All multi-byte values are little-endian. Column order follows
`MATRIX_COL_PINS`; for finger rows the columns are S, E, D, N, W, (unused),
for the thumb row OL, OU, D, IL, MODE, DOUBLE.
