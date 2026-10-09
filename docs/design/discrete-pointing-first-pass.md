> Superseded setup: use the integrated local Keybard preview described in [Keybard integration](discrete-pointing-keybard.md). The Python workflow below is retained only as a development reference.

# Discrete pointing: first test build

8 October 2026 · local experimental branch `design/discrete-pointing`.
Target: two PMW3389 trackballs, Windows shortcuts. No remote push or deployment.
This document specifies the **implemented prototype**. The broader
[architecture](discrete-pointing.md) includes capabilities that are not in it.

## Start here

The bundle contains left and right PMW3389 UF2 images, this guide, the binding
tool and its HID helper, and a manifest with hashes. It uses the maintained
`sval` keymap and the existing settings layout. It does not assign any new
keys automatically. Controls remain inactive until explicitly bound and used.

1. Export your current configuration from Keybard and keep your current firmware
   for rollback. This build is based on the inspected `svalboard/qmk` revision
   `9e7bc919872fb0fd4517796de47021bdbc6b3d36`; an unrelated firmware version may
   use a different settings format. Do not reset EEPROM to try these controls.
2. Flash `pointing-pmw3389-left.uf2` to the **left** half and
   `pointing-pmw3389-right.uf2` to the **right** half using your normal BOOTSEL
   procedure. Reconnect both halves. These images contain firmware only, below
   the existing settings region. Neither half has been flashed automatically.
3. Assign Cancel and one hold control to spare physical keys. Start with
   Volume Hold on the pointer you normally use. Keep free scrolling selected
   for the first sensitivity check, then test your usual axis-lock setting.
4. Hold the control key and roll. Release restores ordinary pointer/scroll
   behavior. Toggling works once the corresponding toggle key is assigned.

There is no new Keybard Controls panel in this build. The temporary keycodes
are available through the supplied tool; existing Keybard scroll, DPI, sniper,
natural-scroll and axis-lock settings still apply as described below.

## Assign and restore a binding

Run Python on the computer to which the keyboard is attached. On Windows, use
Windows Python, not WSL Python. Install its `hidapi` package if necessary:

```sh
py -m pip install hidapi
py point_bindings.py list
py point_bindings.py codes
```

Close Keybard while this tool accesses HID. `list` gives the exact serial to
use. Choose the layer, row and column of a spare key from the matrix layout
in `keyboards/svalboard/info.json`; coordinates are zero-based. The example
below uses left index north, layer 0 (`row=1, col=3`). It changes that key, so
substitute the spare position you actually want:

```sh
py point_bindings.py assign --serial YOUR_SERIAL --layer 0 --row 1 --col 3 --binding right-volume-hold --backup volume-key.json
py point_bindings.py restore --serial YOUR_SERIAL --backup volume-key.json
```

Each assignment saves the previous keycode **before** writing and verifies
readback. Give each assignment a new backup filename. Restore verifies the
serial and refuses to overwrite a later unrelated edit. Keep the backup files.
The script only uses ordinary VIA keymap commands; test instrumentation is not
required. Do not use these codes on an unrelated firmware build.

Useful matrix coordinates: left finger rows 1–4, right finger rows 6–9;
centre column 2, north 3, east 1, south 0, west 4, double-down 5.
Left thumb is row 0 and right thumb row 5, with different physical meanings
for the columns. Assign activators directly to physical keys, not macros,
combos, tap dances or mod-tap wrappers in this prototype.

## Bindings and presets

| Control | Left hold / toggle | Right hold / toggle | Motion and output |
|---|---|---|---|
| Volume | `0x7E00` / `0x7E01` | `0x7E08` / `0x7E09` | Up/down → volume up/down consumer usages |
| Arrows | `0x7E02` / `0x7E03` | `0x7E0A` / `0x7E0B` | Left/right/up/down → corresponding arrow keys |
| Tabs | `0x7E04` / `0x7E05` | `0x7E0C` / `0x7E0D` | Right → Ctrl+Tab; left → Ctrl+Shift+Tab |
| Tasks | `0x7E06` / `0x7E07` | `0x7E0E` / `0x7E0F` | Right → Alt+Tab; left → Alt+Shift+Tab; Alt stays owned until exit |

| Utility | Code | Behavior |
|---|---|---|
| Cancel | `0x7E10` | Disable both controls/toggles; cancel an open task session with Escape |
| Trace | `0x7E11` | Toggle QMK console trace; print recognized/admitted/dropped counts and allowed axes |
| Finer | `0x7E12` | Increase distance per step; affects both pointers, clears partial motion |
| Coarser | `0x7E13` | Decrease distance per step; affects both pointers, clears partial motion |

`point_bindings.py codes` prints the matching names, such as `left-tasks-toggle`,
`right-arrows-hold`, `cancel` and `finer`. These use the unused keyboard-specific
range in this branch, not the existing Sval user-keycode slots. They are an
experimental allocation, not the proposed configurable binding-table ABI.

## Motion contract

- Normalize oriented sensor motion to counts at 800 CPI using signed Q16.16
  arithmetic and independent fractional remainders per pointer and axis.
  The control never changes the sensor CPI. CPI/sniper changes clear partial
  control motion instead of reinterpreting it.
- Preset distances: volume **64**, arrows **32**, tabs/tasks **96** normalized
  counts per step. Finer/Coarser choose 0.5×, 1×, 2× or 4× these distances;
  startup is 1×. This adjustment is RAM-only and resets on reboot.
- Sniper increases motion needed per step. Boost is deliberately excluded from
  step gain in this first pass. Existing legacy scaling continues to influence
  the shared axis-policy input, exactly where it already did.
- One full distance earns the first step. Retain excess forward travel. An
  opposite excursion under **8 counts** must be repaid before forward progress
  resumes. At 8 counts, recognize reversal, discard old partial progress and
  credit the reverse excursion toward the next step. This is sign reversal
  within an eligible axis; it never selects or locks an X/Y axis.
- **250 ms** without motion clears the step recognizer's partial progress.
  It does not disable a toggle, finish a task session or independently unlock
  an axis. The shared scrolling policy owns axis release.
- Each pointer has one pending step, fresh for **80 ms**. Coalesce extra steps,
  not a long replay queue. Pulse starts are at least **80 ms** apart for volume,
  arrows and tabs, and **160 ms** for tasks. Some steps are intentionally lost
  on a fast roll. Admission uses round-robin pointer arbitration.
- In free mode, four-way diagonals recognize both axes. The one-slot limiter
  may coalesce them; alternating tie order avoids a permanent horizontal or
  vertical preference. This is a first-pass rate tradeoff to evaluate, not a
  second axis lock. Trace reports recognized steps separately from admissions.
- Buffer at most 24 input samples per pointer during a shared scroll interval,
  preserving their order and timestamps. Drop excess samples rather than
  allowing an unbounded queue. Pending samples are invalidated on mode changes.
- Control motion produces no cursor movement or ordinary wheel deltas, and
  does not activate auto mouse. A one-axis control consumes both sensor axes.
  Ordinary motion on the other pointer keeps its existing behavior unless an
  exclusive task session is open.

## One shared scroll policy

`scroll_policy.c` contains the extracted existing axis state machine. Both
pointers, wheel scrolling and controls use the **same instance**. There is no
per-control axis lock setting or new direction gate.

Both axes of participating inputs feed the existing normalized scroll-intent
accumulator, with its existing conversion and batching. Output geometry is
applied only after the policy chooses eligible axes. Suppressed movement earns
no steps. Axis eligibility changes clear partial recognition and pending work.
A breakaway batch is discarded for both wheel and control output, as the old
wheel path did. Changing control sensitivity cannot change the lock decision.

The existing asymmetric acquisition is preserved: vertical dominance uses the
existing comparison; horizontal acquisition uses the existing 6250 threshold;
orthogonal breakaway uses 18750. These are internal scroll-policy units, not
the normalized counts used for step distance. Free scrolling permits both axes.
The saved lock preference is effectively bypassed on macOS/iOS exactly as in
the current scrolling path. This build's shortcut presets are still Windows
presets; the bypass does not make Alt+Tab a macOS task-switch shortcut.

Changing from wheel to Volume/Tabs/Tasks does not reset the shared lock. For
example, horizontal Tabs waits for the existing horizontal breakaway if the
lock currently admits vertical motion. Motion on the other scrolling pointer
can influence this same lock. This preserves the existing combined scope.

**One opt-in scheduling correction:** in this experimental build, a stopped
sensor still flushes the last wheel/control batch after the existing >10 ms
interval and delivers zero-motion samples to the existing >500 ms lock timeout.
The old early return prevented that in ordinary idle polling. The lock algorithm
and thresholds are unchanged; the correction is shared by wheel and controls.
Default builds retain their prior callback scheduling. Tests cover both the
extracted algorithm's equivalence and this deliberate integration difference.

Natural scrolling still flips wheel direction once. It does not change the
meaning of “up increases volume,” arrow directions or tab/task navigation.

## Activation and task-session contract

Hold overrides toggle; newest held binding wins. Releasing an older hold does
not disturb a newer one. Releasing the winning hold restores the remaining
hold or toggle. Twelve simultaneous hold tokens are supported; excess holds
are consumed without changing the current activation. Release is matched by
physical position even if the active layer or mapping changed in between.
Multiple activations of the same preset keep that effective preset continuous.

Task switching is lazy: pressing its hold/toggle key alone emits nothing.
The first admitted step opens and advances the switcher. Subsequent steps
pulse Tab with Alt retained; reverse pulses add Shift temporarily.

| Exit/interruption | Implemented behavior |
|---|---|
| Last effective Tasks hold released or Tasks toggled off | Release owned Alt and accept selection |
| Enter or primary mouse button | Accept and consume that key/button press and release |
| Escape, non-primary mouse button, or Cancel | Send Escape with owned Alt, then release; consume the exit input |
| Another control becomes effective | Finish current owned output, accept task selection, start the replacement with fresh motion |
| Any unrelated physical key | Accept before forwarding the key; no synthetic Alt is added to typing |
| Macro/other producer changes keyboard report | Remove synthetic contribution before composing the new base report; deactivate the task control |
| Ten seconds without a new task step | Request cancellation and deactivate task control |
| USB unavailable/suspended, protocol changes, or a previously connected split half disappears | Clear activations and release owned output; retry neutral output after USB returns |

Explicitly accepting/canceling a held task control inhibits it until its hold
is released or its effective control changes. Canceling a toggle clears it.
An open task session suppresses both pointers' ordinary cursor/wheel output
and other controls; only the owning pointer can advance the task session.
The split-disconnect check uses the existing presence monitor, not a new
immediate disconnect guarantee.

**Difference from the target design:** unrelated typing commits rather than
attempting to cancel and replay input into the previous window. This avoids
claiming a reliable focus rollback that firmware cannot guarantee. Confirm
Escape behavior with your Windows task switcher before relying on cancellation.

## Output ownership and transport

`point_output.c` stores ordinary QMK keyboard/NKRO/consumer reports separately
from one synthetic contribution. It composes copies at the ChibiOS send seam.
It never adds/removes its modifiers in QMK's real or weak modifier bitmasks.

For this first pass, a new pulse requires an empty base keyboard report
(including modifiers) and no held consumer usage. Thus physically held Shift
**blocks** arrows rather than extending selection; inherited modifiers remain
a future feature. A new base report preempts a conflicting synthetic pulse
before composition, preserving the actual physical/macro report. The consumer
adapter restores the current base usage rather than unconditionally sending 0.

Down/up phases advance only after the ChibiOS queue accepts their reports.
An uninterrupted accepted down lasts at least 20 ms, followed by a neutral gap
of at least 20 ms. Explicit exit or conflicting input can release it sooner.
An unaccepted down expires after 80 ms. Releases remain pending and are retried;
new synthetic presses cannot overtake them. Cleanup runs from housekeeping,
independent of new pointer motion. Releasing/changing a control invalidates
its not-yet-emitted pulse as well as its pending motion.

This uses the existing ChibiOS `send_report` function and its 100 ms timeout.
It is not the final nonblocking transport API from the architecture. Endpoint
acceptance is not confirmation that the OS processed a shortcut. Real USB
congestion, suspend/resume, boot/NKRO transitions and alternate producers still
need hardware validation; host-side fake-transport tests cannot prove those.

## Suggested first session

1. Verify typing, both cursors and existing wheel scrolling before assigning
   controls. Check free diagonals, then your familiar axis lock and breakaway.
2. Hold Volume; roll slowly, reverse near a step boundary, then release. Try
   Finer/Coarser and your existing sniper keys. Listen for unwanted trailing steps.
3. Toggle Volume, hold Tabs over it, release Tabs, then toggle Volume off. Check
   that each restored control starts with fresh partial motion.
4. In a text editor, test Arrows with free scrolling and with axis lock. Check
   both diagonal behavior and the cost of changing axes. Held Shift currently
   blocks synthetic arrows; release it to resume.
5. With two harmless windows open, hold Tasks and move right, then left. Release
   to accept. Repeat with Escape, Enter, primary/secondary click, and toggle.
   Try an unrelated letter: it commits and types into the selected window.
6. Test one pointer scrolling while the other operates a control. Their common
   axis lock should behave as one system. During Tasks, the other pointer must
   not move the cursor or operate another control.
7. Enable Trace if needed, capture the QMK console, and note the control,
   pointer, CPI, free/locked setting, distance multiplier and what felt wrong.
   Turn tracing off for normal latency/feel evaluation.
8. Use Cancel, restore your test bindings with their backups, and reflash your
   saved normal firmware when finished. Keep configuration exports until you
   have verified the restored setup.

## Build and validation

From the repository root:

```sh
make svalboard/trackball/pmw3389/left:sval SVAL_POINT_CONTROLS=yes -j6
make svalboard/trackball/pmw3389/right:sval SVAL_POINT_CONTROLS=yes -j6
python3 -m unittest discover -s tests/sval_pointing -v
python3 -m unittest discover -s experiments/discrete_pointing -v
```

The new host tests compile the actual C recognizer, controller, output broker,
and combined pointer adapter. The adapter is extracted directly from the
source for the test, not rewritten. A separate equivalence test compiles the
original pre-change lock function and compares 10,005 samples including timer
wrap, acquisition, breakaway and idle boundaries. The existing Python model
remains an additional independent reference, not firmware execution evidence.

No test board or physical trackball was used to validate this first build.
The supplied UF2 files are compiled previews for your requested PMW3389 pair.
Builds without `SVAL_POINT_CONTROLS=yes` omit the controls and transport hooks.

Still outside this first pass: persistent/custom control tables, a Keybard
editor, configurable shortcuts and per-control tuning, calibration UI,
modifier inheritance, arbitrary macros as outputs, high-resolution wheel
controls, and full nonblocking transport/backpressure integration.
