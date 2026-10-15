# Unified two-hand touchpad (current experiment)

Run `unified_lab.py --serial EXACT_RIGHT_USB_SERIAL --log NEW_FILE.jsonl` with
Windows Python. It opens a live combined preview with **real Windows input off**.
Lift every finger on both pads once to arm. The default geometry is one 86 × 40 mm
surface: left coordinates 0–43 mm, right 43–86 mm. The virtual gap and each pad's
X/Y flips are configurable; changing any setting cancels and rearms the session.
The lower panel previews pan, scale and rotation when exactly two fingers are
active. There is no requirement to start both fingers simultaneously.

The checkbox **Enable REAL Windows touchpad input** creates one native
`PT_TOUCHPAD` device for both physical sources. One finger on each hand becomes
a normal two-contact gesture; three fingers on either side become a native
three-finger gesture, subject to Windows/app gesture settings. This is a host
synthetic device, not a changed USB HID descriptor. It is still a standalone
lab, not yet installed in the persistent Keybard overlay daemon.

The two sources' `(pad, epoch, lifetime ID)` tuples map to unique synthetic IDs.
The join uses HID-thread receipt timestamps, never subtracts the two MCU clocks,
and requires both pads to report. Samples must be within 25 ms of one another;
a missing heartbeat over 60 ms, invalid frame, reset, contact-ID discontinuity,
or a worker stall cancels the combined gesture. Duplicate packets do not renew
freshness. A gap of at most two omitted source samples is tolerated only when
contact lifetime IDs are unchanged; the latest-snapshot split transport can
legitimately skip intermediate frames. No coordinates are extrapolated. The
remote transport can itself buffer an old snapshot up to its firmware freshness
limit, so host receipt age is not a physical sensor timestamp or latency bound.

Windows permits five simultaneous synthetic touchpad contacts. The preview
shows raw contacts from both sensors, but six or more cancels unified output
until a fresh all-up on both pads; it never silently drops the sixth contact.
The two-pad namespace and readiness rules are tested independently of Windows.

Firmware `unified-pad-dev` (2302) adds stream START mode **2** for the unified
host and returns ownership flag **0x08**. That lease suppresses both halves'
ordinary mouse reports on the USB master, including buttons, so the synthetic
pad is the only output while enabled. The host waits for two-pad all-up before
injecting. Mode 1 remains rejected for the old single-pad injector. STOP or the
500 ms lease timeout restores ordinary output. Both matching halves keep blank
keymaps, the inert physical matrix, and no-chord updates. The split wire layout
is unchanged from the preceding pair firmware.

The app renews its wrapper identity every 45 seconds: it closes the synthetic
device, releases/reacquires capture, and waits for all-up again. This can
interrupt a gesture spanning renewal; seamless ownership transfer is future
work. The checkbox does not persist across launches. Close releases capture;
on errors it disables the checkbox and firmware's watchdog handles host loss.
Ordinary mouse output remains active in preview mode.

Validation: 43 host tests including namespace collisions, cross-hand pinch,
stationary contacts, real lifts, duplicate/stale reports, epochs, small gaps,
sequence wrap, geometry, and five/six-contact handling; ASan/UBSan split tests
including mode-2 lease expiry; matching ARM split tables and RAM commit checks;
verified update of both halves through the right USB; blank keymap readback;
Windows live-preview smoke; native 86 × 40 mm device creation and capture
acquire/release without injecting contacts. Physical Windows gesture behavior
still needs interactive trials. The pair currently has intermittent invalid
sensor frames: those cancel rather than fabricate releases/taps.

[Microsoft's synthetic touchpad API and five-contact limit](https://learn.microsoft.com/en-us/windows/win32/api/winuser/ns-winuser-synthetic_device_creation_params)

## Earlier split-capture implementation (historical)

The following describes version 2301, prior to unified capture mode and host UI.

# Permanent split-touchpad test firmware

This branch prepares two keyless Azoteq controllers for a split pair with one
USB connection. Both halves must run these matching images: the added
`KEYBOARD_MT` RPC changes the split transaction count and uses a 64-byte
slave-to-master buffer. The updater protocol itself is unchanged.

Build each side with:

```sh
make svalboard/azoteq/left:blank SVAL_UPDATER=yes SVAL_UPDATE_NO_CHORD=yes SVAL_AZOTEQ_MULTITOUCH=yes SVAL_TOUCH_TEST_BOARD=yes SVAL_FW_VERSION=2301 SVAL_FW_VERSION_STRING=touchpair-dev -j6
# Repeat with svalboard/azoteq/right:blank.
```

`SVAL_TOUCH_TEST_BOARD=yes` requires the blank keymap, updater, and no-chord
updates; release builds are rejected. It also skips physical key matrix reads,
so floating inputs cannot type even if a dynamic binding is accidentally added.
Keep these fixtures on blank builds. Firmware updates preserve board names and
settings; no reset token is used here. Reconnect/update the two boards separately
before joining them, since old and new split tables are incompatible.

The satellite samples its own pad and atomically publishes a complete cached
56-byte frame. The USB half polls it at most every 10 ms over one 60-byte RPC
reply (header, source-measured age, frame). The callback performs no I2C and
waits for no sensor. Repeated sequence numbers are suppressed; stale data or a
lost link produces an invalidation after 100 ms. Recovery is flagged as a
stream discontinuity. A bounded shared USB queue carries both sources using
the existing 64-byte Raw HID frame format, with pad 0=left and pad 1=right.

This is a latest-snapshot transport, not a guarantee to deliver every sensor
sample. A slow master may skip intermediate frames. Contact lifetime IDs are
assigned on each source, and sequence gaps can be measured independently per
pad. Timestamps are from independent MCU clocks and must not be directly
subtracted across hands. Sensor characterization commands currently target
only the USB-local sensor. A remote reconnect/reboot, clock alignment, bandwidth
under touch, and physical split latency still need hardware characterization.

This pair prototype is **observation-only**: firmware refuses exclusive relay
mode and this checkout's capture tool rejects `--relay`. The old single-pad
Windows injector must not merge the two pad namespaces. Ordinary mouse behavior
remains enabled. Follow-up host integration needs separate per-pad guards,
watchdogs, epoch/sequence tracking, and explicit two-hand gesture arbitration.

```sh
python capture.py --serial EXACT_USB_SERIAL --report-size 64 --seconds 15 --diagnostics --output NEW_CAPTURE.jsonl
```

The capture summary includes per-pad frame/contact counts and tracks sequence
gaps separately. Captures remain bounded to 45 seconds because this older
stream client does not renew its wrapper identity. Do not use the single-pad
renderer/injector as if these paired frames shared one contact namespace.

Validation for this change: ASan/UBSan host tests of the actual split stream
code (coherent snapshot, deduplication, freshness, link recovery, source side,
update exclusion, relay rejection, queue bounds); 31 host tests; four sensor
driver tests; both ARM builds; matching 16-entry split transaction tables; and
RAM-only commit checks on both images. LeakSanitizer is disabled for the host
harness because the sandbox uses ptrace. Physical validation so far is the
left board's local sensor: 482 idle frames over five seconds, zero sequence
gaps and sensor errors. The right image is prepared but not flashed, and no
physical split-pair validation has happened yet.

## Earlier local-only prototype notes (historical)

The notes below describe the earlier firmware and injection experiments, not
this pair build's current capabilities or installation state.

# Windows Azoteq multitouch prototype

This opt-in prototype carries all five Azoteq contact slots from the USB-connected
half to the existing Keybard Host. Windows receives a synthetic **touchpad**, using
its gesture engine. It does not emulate a touchscreen or implement gestures with
keyboard shortcuts. See the [architecture](../../docs/azoteq-multitouch-architecture.md).

## Components

- Firmware: one coherent 45-byte sensor read, five slot/lifetime identities,
  complete-frame queue, experimental wrapped Raw HID protocol `0xFC`.
- `transport.py`: one HID I/O worker dispatches command replies and stream frames;
  reassembly rejects incomplete/stale frames. Input waits for physical all-up after
  discontinuities. A synthetic device is destroyed/recreated on abnormal input.
- `windows_touchpad.py`: dynamic User32 binding to `CreateSyntheticPointerDevice2`
  and `InjectSyntheticPointerInput`, with `PT_TOUCHPAD` and physical dimensions.
- `session.py`: input worker shared with Keybard's existing device worker. The
  overlay's rendering thread is outside the input path.
- `windows_probe.py`: creates an isolated target window, injects a two-finger pan,
  verifies scroll delivery and touchpad pointer delivery, then closes it.
- `capture.py`: exact-serial observation and optional Windows relay, with JSONL
  capture and a summary. Observation leaves legacy mouse output enabled.
- `keybard_probe.py`: exercises the actual Keybard profile/layer reader while
  receiving frames through the same dispatcher and HID handle.

## Build and run

Build in WSL, targeting the half that actually has the pad and USB connection:

```sh
make svalboard/azoteq/right:blank SVAL_AZOTEQ_MULTITOUCH=yes SVAL_TEST_WIPE=yes -j4
python3 -m unittest discover -s keyboards/svalboard/tools/multitouch -p 'test_*.py'
make test:pointing/azoteq_multitouch
make test:pointing/azoteq_iqs5xx
```

Back up the exact test board before flashing. The option also enables the existing
host bootloader command for recovery. The prototype assumes TPS43 geometry,
2048 × 1792 coordinates and a 43 × 40 mm surface. CPI changes are disabled in this
build to keep the absolute domain fixed. Normal builds retain the existing driver.

For this bare-MCU test board, use the **blank keymap**, as explicitly requested by
the user. The earlier `:sval` build generated unwanted keyboard activity and
confounds its load/error measurements. Keep the prototype on this board after
testing; the user explicitly requested no automatic firmware restoration.

The latest flashed build uses base `cc6188ee23` (`feat/hid64-pointing-auto`) in
the isolated checkout `.local/multitouch-latest`, with the prototype ported there.
That base implements `SVAL_TEST_WIPE=yes`: on transition from normal firmware it
resets saved settings to the build defaults. With `:blank`, all keys are unmapped.
The older base of this main checkout does not implement that wipe option.
The current board uses **64-byte HID reports**: pass `--report-size 64` to
`capture.py`, or set `KEYBARD_TOUCH_REPORT_SIZE=64` for the Keybard integration.
The revised 64-byte stream carries one complete frame per report (opcode `0x41`).
The 32-byte firmware path retains three-fragment reports (opcode `0x40`).
Command replies retain their existing first 32 logical bytes.
Use `capture.py --diagnostics` with the revised firmware to record separate read
timeout/error counters, session-close timeout/error counters and maximum acquisition
duration. The prototype selects 400 kHz I²C and a 30 ms deadline for contact reads;
other sensor operations retain the existing 10 ms deadline. This remains a
synchronous acquisition path: production scheduling should use sensor readiness
or a separate acquisition worker to avoid delaying keyboard scans during stretching.

To visualize a recording without hardware or Windows, generate an offline replay:

```sh
python3 render_capture.py contacts.jsonl touch-replay.html
```

Open the resulting HTML in a browser. It includes playback, speed controls,
scrubbing, contact-count shortcuts, raw contact values and a fault timeline.
Trails never bridge sequence gaps, invalid frames, epoch changes or contact reuse.

Run these with **Windows Python**, with `hidapi` installed; Tkinter is required for
the delivery test. A WSL UNC path to the script is acceptable.

```powershell
python windows_probe.py --delivery-test --output windows-probe.json
python capture.py --serial 'sval:EXACT_FLASH_ID' --seconds 30 --output contacts.jsonl
python capture.py --serial 'sval:EXACT_FLASH_ID' --seconds 30 --output relay.jsonl --relay
python keybard_probe.py --serial 'sval:EXACT_FLASH_ID' --keybard C:\path\to\keybard --output keybard-probe.json
```

`--relay` explicitly takes mouse-output ownership; contacts can operate the desktop.
Use observation first to verify coordinates and contact counts. Output files must
be new. Sessions last at most 45 seconds; reconnect/restart to repeat.

The installer narrowly adds the input worker to a Keybard checkout, preserving
existing edits and refusing unexpected/already-patched source layouts:

```sh
python3 install_keybard.py /path/to/keybard          # dry run
python3 install_keybard.py /path/to/keybard --apply
```

Launch Keybard Host normally with `KEYBARD_TOUCH_SERIAL` set to the exact selected
board. Default mode observes; set `KEYBARD_TOUCH_MODE=relay` to inject. Other serials
are unaffected. The existing checkout has already received this opt-in integration.
This is a bounded development session, not a persistent daemon feature yet.

## Wire format and ownership

All multibyte stream values are little-endian. Requests use the existing client
wrapper: `DD client:u32 FC op mode token:u16`, zero-padded to 32 bytes. Operations
are `0=info`, `1=start`, `2=renew`, `3=stop`; mode `0=observe`, `1=relay`. Start returns
a session token. Another client cannot steal an active lease.

Replies retain the wrapper and contain: `80|op`, status, version, ownership flags,
token:u16, xmax:u16, ymax:u16, width-mm:u8, height-mm:u8, max-contacts:u8,
samples:u32, errors:u32, dropped:u32. Ownership bits are active=1, requested=2,
waiting-for-all-up-after-release=4. Heartbeats renew every 100 ms; firmware expires
the lease at 500 ms and waits for physical all-up before restoring mouse output.

Stream packets: `DD client:u32 FC 40 token:u16 sequence:u16 fragment:u8 length:u8`
followed by 19, 19, or 18 payload bytes. A frame is 56 bytes:

On a 64-byte interface, the compact packet is `DD client:u32 FC 41 token:u16`
followed by bytes 1–55 of the frame. Opcode `41` implies frame version 1; the host
prepends that byte before decoding. Every report is independently complete, so
there is no fragment reassembly or partial-frame loss on this path. Firmware drains
up to three queued reports per housekeeping pass without waiting for USB space.

Diagnostic operation `4` returns `DD client:u32 FC 84 status:u8`, followed by five
u32 counters: read timeouts, other read errors, session-close timeouts, other
session-close errors, maximum acquisition duration in milliseconds. Counters are
cumulative since boot; compare before/after values for each capture.

| Offset | Contents |
|---|---|
| 0 | Version:u8, pad:u8, flags:u8, active slot mask:u8 |
| 4 | Sequence:u16, frame size:u16 |
| 8 | Acquisition timestamp:u32 (microseconds, millisecond resolution), epoch:u32 |
| 16 | Five 8-byte records: lifetime ID:u8, X:u16, Y:u16, strength:u16, area:u8 |

Flags: palm=1, excess fingers=2, invalid read=4, reset/re-ATI/ATI error=8,
overflow/identity restart=16, relay owns output=128. Slot strength determines
presence only when finger count is nonzero; this excludes the sensor's tap residue.
Zero coordinates are valid. Contact IDs change when a slot is reused. Epoch changes
invalidate the entire gesture. Coordinates in inactive records are unspecified.

Firmware streaming posts complete USB packets without waiting for endpoint space.
QMK's generic `TIME_IMMEDIATE` send still resets/retries its queue on exhaustion;
the prototype instead posts atomically to the ChibiOS endpoint queue. This is
platform-specific code requiring review before an upstream abstraction.

## Validation and remaining work

### Guided characterization and two-hand lab

`characterize.py --serial EXACT_SERIAL --output NEW_DIRECTORY` runs a Windows Tk
test with one-finger and three-finger groups, varying the allowed contact count
and requested report period. It uses the 64-byte observe stream and volatile
configuration commands; normal cleanup and the firmware lease watchdog restore
five contacts / 10 ms. Use the latest `prototype-characterize.uf2` build.
`render_characterization.py DIRECTORY report.html` renders the measurements.
See the latest results below for the distinction between detected-contact loss,
sensor report-rate misses, and transport loss.

`python manipulation_lab.py` opens a Windows-compatible two-hand simulation.
Hold the **Clutch held** checkbox and drag a dot on each pad, or use Pan, Pinch
and Rotate presets. Each source contributes one vector; the virtual touchpad uses
100 × 100 mm coordinates with initial contacts 20 mm apart. The view displays the
shared midpoint motion, distance ratio and angle. A dropout button demonstrates
cancellation and the need to release/reapply the clutch. Trackballs mode integrates
relative deltas into the same engine, including zero-motion heartbeat requirements.
This lab has no hardware access and does not inject desktop input.

`gestures.py` supplies the reusable `TwoHand`, `ThreeFingerSwipe` and `BallVectors`
engines. Input is per-hand `Sample` data in millimetres, with a monotonic host time,
16-bit sequence and stream epoch. Two-hand output includes stable virtual contact
IDs, pan in mm, scale and angle in degrees. A future Windows adapter can supply
those contacts, multiplied by 100 for HIMETRIC, to
`WindowsTouchpad(width=10000, height=10000)`. Clutch release ends the contact set;
faults must use backend cancellation. No live two-pad route is installed yet.
Three-finger recognition requires coherent movement of three contacts on one
hand, emits once until all-up, and rejects other-hand activity and contact loss.
It is a tested recognizer prototype, not a replacement for the existing native
Windows gesture relay and not an automatically enabled app-switching shortcut.

Unit tests cover motion geometry, sequence wrap, gaps, stale/duplicate input,
contact loss between render ticks, lifetime changes, clutch rearming, swipe
coherence and ball reanchoring. Physical dual-pad and dual-ball behavior remains
unmeasured; the current test hardware has one pad.

See [recorded results](RESULTS.md). Windows API availability is checked dynamically;
the tested Windows build is 26200. Older Windows support is not established.
Microsoft's [Precision Touchpad guide](https://learn.microsoft.com/en-us/windows/win32/input-precisiontouchpad/precision-touchpad-guide)
defines the API's touchpad-specific state flags and HIMETRIC coordinates.

Remaining product work includes split-half contact transport, runtime geometry and
orientation negotiation, sensor reconfiguration after unexpected reset, persistent
client renewal/reconnect, native USB Precision Touchpad comparison, Linux uinput,
firmware queue/lease fault-injection tests, and sustained latency/gesture testing.
Palm policy currently cancels the whole gesture. Physical buttons and per-contact
palm classification are not represented. Five simultaneous physical fingers must
be measured; a five-slot parser is not proof of five-finger hardware performance.

## Unified pair reliability diagnostics

`unified_lab.py --serial EXACT_SERIAL --log NEW.jsonl` previews the split pair
as one surface; real Windows input remains an explicit checkbox. Logs contain
raw frames with HID receipt timestamps as well as merged states and per-cause
fault counts. The UI counts canceled **active gestures** separately from faults
while idle. A flagged frame and its associated epoch reset are one event.
Flag `0x04` is sensor acquisition invalid; `0x20` is a stale split snapshot.
Faults still require all-up rearming; these distinctions do not relax safety.

Firmware 2305 preserves RP2040 I2C address-NACK diagnostics that the underlying
ChibiOS driver discarded. One pure address NACK permits one coherent-read retry
following a 200 µs delay. Timeouts, data NACKs, arbitration loss, other bus faults,
and failed communication-window closure still invalidate the acquisition.
`capture.py --diagnostics --retry-diagnostics` reads ordinary acquisition counters
and opcode 8 retry counters plus opcode 9 raw RP2040 abort status before/after capture. Retry diagnostics are local to
the USB-connected MCU; remote failures are visible through its streamed flags.
The patched HAL is a prototype-local copy, selected only for this build option.

## Native Precision Touchpad (experiment/native-ptp)

`SVAL_NATIVE_PTP=yes` (with `SVAL_AZOTEQ_MULTITOUCH=yes` and
`SVAL_TOUCH_TEST_BOARD=yes`) makes the USB half present both pads as one
Windows Precision Touchpad on its own HID interface (Digitizers 0x0D, Touch
Pad 0x05). Windows, or Linux `hid-multitouch` with libinput, then owns the
gestures; no host app runs. The firmware side is `azoteq/ptp.c`, with the
descriptor in `azoteq/ptp_descriptor.h` and a small `tmk_core` patch behind
the same flag. Without the flag the build is byte-identical to before.

One owner at a time, chosen explicitly: `ptp_owner.py --serial EXACT_SERIAL
{get,mouse,host,native}`. Native builds start as `native`; the host-injection
lease (`unified_lab.py` and friends) is refused (status 5) until the owner is
`host`. The setting is RAM only.

- `ptp_desc_check.py BUILD.elf`: offline check of the descriptor in a built
  ELF (page 0xFF00 blob, report sizes 35/2/257/2/2/2, 5 contacts, X/Y ranges,
  interface and endpoint numbers, bcdDevice).
- `ptp_hid.py`: the small HID descriptor parser both use.
- Host tests: `util/test_native_ptp.sh` (unifier, feature reports, owner
  command, descriptor; hid-tools cross-check when it is installed).

## Timing counters (SVAL_PERF_STATS)

 (with ) adds
: per-half counters of main-loop passes, matrix frames, pad
I2C reads (whole read and end session, in microseconds) and, on the USB
half, the KEYBOARD_MT RPC to the other half, with maximums and gap
histograms (<1, 1-2, 2-4, 4-8, 8-16, >=16 ms). Raw HID 0xFC op 0x43 reads
them (data[7] 0 this half, 1 the other half, 2 reset both; data[8] page
0-2); the other half answers KEYBOARD_MT requests 0x10|page and 0x20. With
the flag off the firmware is byte-identical to the build without it.

    python perf.py --serial SERIAL run 30     # one 30 s window, both halves
    python perf.py --serial SERIAL watch 10   # one line per second
