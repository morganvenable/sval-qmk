# Native touchpad

With an Azoteq trackpad on either hand, Svalboard presents its pads to the
computer as one Precision Touchpad on its own USB interface. Windows, and Linux
with libinput, then handle every gesture themselves: pointer, tap and
tap-and-drag, two-finger scroll with inertia, pinch, two-finger right click,
and three- and four-finger swipes. No app runs on the computer.

## What the user gets

- **Windows and Linux** switch the touchpad interface to touchpad mode (HID
  Input Mode 3) when it connects. From then on the operating system's
  touchpad settings apply to the pads, and Keybard's pointer settings (DPI,
  scroll mode, sniper, natural scroll, axis lock) do not apply to a pad hand.
  The firmware reports this per hand in the Sval pointing identity (0x2C):
  flag `SVAL_POINTER_OS_TOUCHPAD` (0x10), so Keybard can show it.
- **macOS, a BIOS, or any host that never asks for touchpad mode** gets the
  pads as today's mouse, with the trackpad chip's own tap, scroll and drag.
- **A trackball or TrackPoint on the other hand** works exactly as before: only
  a hand whose module is an Azoteq pad is ever handed to the touchpad.
- **No pad on either hand:** the interface is still there, but it never
  reports and nothing else changes.
- **Linux:** libinput turns tap-to-click off by default for touchpads with
  buttons; turn it on in the desktop's touchpad settings.

The two pads, one per hand, appear as one surface: the left pad, a 10 mm
unreachable gap, then the right pad, 96 × 40 mm in all (4572 × 1792 counts). A
board with one pad uses the same surface, with the pad in its hand's part.

## How it works

| Piece | File |
|---|---|
| Make flags | `azoteq/multitouch.mk` (included by `pointing_auto/pointing_auto.mk` and `azoteq/rules.mk`) |
| Build settings | `azoteq/ptp_config.h` |
| Report descriptor | `azoteq/ptp_descriptor.h` |
| Contacts, frames, feature reports | `azoteq/ptp.c` |
| Pad reads, the other half's pad, the dev command | `azoteq/multitouch.c` |
| Read timing | `azoteq/pad_schedule.h` |
| Contact read in the driver | `drivers/sensors/azoteq_iqs5xx.c` (behind `SVAL_AZOTEQ_MULTITOUCH`) |
| USB interface | `tmk_core/protocol/usb_ptp.h`, `usb_descriptor.c/.h`, `chibios/usb_main.c`, `chibios/usb_endpoints.c/.h` (behind `SVAL_NATIVE_PTP`) |

**Interface.** The touchpad is the last USB interface, so every other
interface keeps its number, with one 64-byte interrupt IN endpoint polled every
1 ms. Its feature reports (the 257-byte certification blob, Input Mode,
Surface and Button Switch, latency) are answered by `ptp.c` through one hook in
`usb_main.c`. The build sets bcdDevice to 0x0F03 so Windows reads the new
interface set instead of a cached one.

**Pads.** Each half reads its pad at the chip's 8 ms report period, over I2C at
400 kHz, in one read: the 10 base bytes and the 7-byte contact slots the last
read needed (up to the highest slot with a contact, since a contact keeps its
slot until it lifts). Only when the chip's count says more contacts are down
than that read found does a second read, in the same communication window,
fetch the remaining slots; so a read grows with the contacts down, and costs
two reads only as a finger lands. The USB half feeds its own pad to
`ptp.c` at once. The other half keeps its last sample; the USB half fetches it
over the split link on the Scan Lab RPC with its own request op (0xA7, 32-byte
reply), so the split message table is the same as in every other build. The
other half marks its split pointing report with a counter that moves only when
its pad has news, and only once the USB half has asked for it; the USB half
fetches on news, every 10 ms while that pad has contacts, and every 250 ms
while it is quiet.

**Read timing.** A read that starts before the chip's window opens is held by
clock stretching, and the main loop with it. `pad_schedule.h` times the next
read from the end of the last one, one period later less a lead that adapts so
that reads land just after the window opens. The bare transfer time it
subtracts is kept per read length (0 to 5 slots), so a longer read is not
taken for a stretch. A pad that stops answering is read less and less often,
down to about once a second.

**Contacts.** Contact IDs, the 5-contact limit (the most a Windows Precision
Touchpad allows), arming on an all-up sample, palm, faults and lifts are in
`ptp.c`. A finger beyond the five is left out of the touchpad for its
lifetime, but `POINTING_CONTACTS` (0x2F) still shows it, up to 6 fingers in
all, so a host app draws three on each pad. A pad whose contacts get no valid
sample for 50 ms is lifted; so is the other pad while an update runs. With
contacts on both pads, a frame waits up to one period for the second pad's
sample, and every frame is stamped with its newest sample's read time.

**The other pad over a flaky link.** A failed split transaction blocks the
USB half for QMK's split timeout (20 ms), and on the touchpad test board they
come in bursts while both pads are touched. Windows ends a pinch as soon as
either contact lifts, so the other half's contacts are held at their last
position for up to 150 ms without a new sample (`SVAL_PTP_REMOTE_STALE_MS`)
before they are lifted, and a reply with no sample from a half that has been
sending them counts as a failed fetch until three come in a row (QMK's RPC
reply carries no checksum). A pad lifted because its samples stopped arms
again on its next valid sample, so a finger still down is reported again as a
new contact instead of staying unseen until it lifts; a chip reset or palm
still waits for an all-up sample. The dev command's status (v3) counts these
lifts per pad.

While Velocity Scroll is on (`SV_VELOCITY_SCROLL` held, or
`SV_VELOCITY_SCROLL_TOGGLE` tapped on; `velocity_scroll.h`), the firmware takes
the pads from the operating system: reports stop after a
release of every contact, and `ptp.c` follows one finger per pad for the
scroll. A finger already down is followed at once; after it lifts, only a
finger that lands later is, so one resting on a pad never takes over. The OS
sees the fingers lift with Confidence clear, so a finger that had just
landed is never read as a tap. When it turns off, a finger still down goes
back to the OS at once, as a new contact where it is (its last sample is fed
again in the same pass); if it lifts within 300 ms
(`SVAL_PTP_HANDBACK_TAP_MS`) its lift goes out with Confidence clear, so the
hand-back is never a tap either.

### Pointer modes on a pad (prototype, dev builds)

Dev builds (`SVAL_TOUCH_DEV`) have selectors, held in RAM and Off at boot,
that let the firmware's pointer keys work on a pad the OS owns. They change
nothing on a Mac, with built-in gestures, or with the touchpad off: there the
pad is the chip's mouse and the keys work on it as always.

- **Scroll toggle: Takeover.** While the scroll toggle or hold key has a pad
  hand scrolling (`SV_SCROLL_TOGGLE`, `SV_SCROLL_HOLD`; the hold key flips
  the toggle, as on a trackball), the firmware takes the pads with the same
  grab as Velocity Scroll and turns one finger's motion into the firmware's
  scroll, at the hand's DPI, with the scroll divisor, natural scroll and axis
  lock a trackball hand gets. The hand's saved scroll setting does not apply
  to a pad (the OS owned it); only the keys do.
- **Sniper and boost: Takeover.** While any sniper or boost key is on (hold
  or toggle) and its selector is Takeover, the firmware takes the pads the
  same way and drives the cursor from one finger through the firmware mouse
  path: the hand's DPI, then the sniper divisor and the boost multiplier, as
  on a trackball. While the firmware holds the pads the OS gets nothing from
  them: no taps or tap-and-drag, no two-finger scroll, pinch or swipes,
  and no OS pointer acceleration (the firmware motion
  goes out as a mouse, so the OS mouse speed and "enhance pointer precision"
  apply instead of the touchpad's curve). A second finger is ignored. When
  the key is let go, the finger goes back to the OS at once, as above: the
  cursor keeps moving, with one sample of the new contact's touch-down.
- **Sniper and boost: Scaling.** The OS keeps the pads (taps, gestures and
  its curve stay). While the key is on and exactly one contact is down, that
  contact is reported at its anchor plus its travel since then, times the
  factor (sniper 1/divisor, boost multiplier; both multiply), clamped to the
  whole surface the OS sees (both pads and the gap). It starts when the key
  comes on (again) with the contact alone, or when a finger lands alone while
  the key is on. When the key is let go or a second finger lands, the contact
  goes on at 1x from where it is, keeping its offset: no snap back, and no
  lift and re-touch (which the OS could read as a tap).
- **Sniper and boost: Frame.** The same, for every contact: while the key is
  on, every contact's travel is scaled. A finger that lands enters the frame
  where the first finger's map puts it, so the fingers' reported spacing stays
  consistent. A press or release re-anchors every contact where it is
  reported, so nothing already down jumps (the spacing at the press is kept;
  the motion after it is scaled).

A scaled point that reaches the surface's edge slides along it: that axis
stops at the edge and is anchored there, so the other axis keeps moving and
turning back moves at once. Every stroke's first finger starts at its own real
position. The host app's contacts (0x2F) always show where each finger really
is, on its own pad, never the scaled or offset position the OS gets.

In every mode, a press or release mid-stroke changes the speed from the next
sample: the transition sample still uses the old factor, then every contact
is anchored where it was just reported. No lift is needed, nothing jumps, and
nothing stalls.
- **Surface: Large.** The touchpad tells the OS its surface is seven times the
  standard one on each axis (32004 x 12544 counts, 672 x 280 mm, within the
  descriptor's signed 16-bit maxima) at the same counts per mm, with the real
  pads, gap included, in its center, so pointer speed and the OS's gesture
  distances stay the same and boost by scaling has room to travel. This one
  is stored (two bytes after the touchpad mode's record) and picks the
  descriptor at boot, so it applies after a restart (action 5) or a replug.
  The pad type is 2 (external buttons, not a clickpad), so Windows has no
  corner or bottom-edge button zone to move; Microsoft's device integration
  guide gives a minimum size (32 x 64 mm) and a recommended one (about 65 x
  105 mm), and no maximum.

The grab has one holder bit per reason (`SVAL_PTP_GRAB_VS`, `_SCROLL`,
`_POINT`; `sval_ptp_set_grab`), worked out again every pointing task from the
modes and the keys, so the pads go back only when the last reason ends, and
the finger followed stays the same from one holder to the next.
`pad_follow.c` turns the followed finger into counts, as the pad's own mouse
output scales them. A finger already down when the grab takes it starts from
where it was a sample before (`sval_ptp_grab_origin`), so that sample's motion
is not lost; one that lands starts where it lands. Velocity Scroll still
has precedence while it is on.

Selectors and test holds go over the dev command (raw HID 0xFC op 0x42):
action 3 gets or sets the modes and the surface ([8] 0 get, 1 set; [9]
scroll 0 Off 1 Takeover, [10] sniper and [11] boost 0 Off 1 Takeover 2 Scaling
3 Frame, [12] surface), action 4 holds their keys on a
keyless board ([8] bit 0 scroll, 1 sniper /2, 2 boost x2; lapses 500 ms after
the last repeat; `tools/multitouch/pad_hold.py`), action 5 restarts the board
200 ms after its reply, and every reply carries the modes' block at bytes 56
to 63: version 1, scroll, sniper, boost, surface stored, surface in force,
supported (bit 0 scroll Takeover, 1 sniper and boost Takeover, 2 their
Scaling, 3 the large surface, 4 their Frame), and the grab's holders.

## The split link's receive (dev builds, prototype)

Failed split transactions (the gaps on the other half's pad, and lost motion
from a trackball on the half without USB) are overflows of the USB half's
8-byte PIO receive FIFO: the bytes of a reply are drained by the main thread,
one interrupt and wake per byte, and an interrupt handler (USB up to about 300
us, measured with `SVAL_IRQ_TIMING`) can hold it off past the 80 us those 8
bytes take at 1 Mbaud. The state machine then stalls and misses bits, and the
transaction fails (often after the 20 ms receive timeout).

`SVAL_SPLIT_DMA=yes` (dev builds) drains the USB half's receive FIFO by DMA
into a ring, so no wake per byte is needed, fails a receive at once on a
framing error, and gives QMK's per-scan transactions (and the pad snapshot
fetch) a 3 ms deadline while the other RPCs, the updater's relay among them,
keep 20 ms. The half without USB keeps the interrupt receive. Dev diagnostics:
0xFC op 0x42 actions 7 (split counters, both halves) and 8 (interrupt timing).

## Builds

| Flag | Where | Effect |
|---|---|---|
| `SVAL_NATIVE_PTP` | on in the auto builds (`svalboard/left`, `svalboard/right`), off in `svalboard/azoteq/*` | The native touchpad. `SVAL_NATIVE_PTP=no` builds without it |
| `SVAL_PTP_KEY_BUTTONS=yes` | with the touchpad | MS_BTN1 and MS_BTN2 are the touchpad's buttons while it reports |
| `SVAL_PTP_MOUSE_BEFORE_MODE3=no` | with the touchpad | The pads stay silent until the host asks for touchpad mode |
| `SVAL_TOUCH_DEV=yes` | dev builds only | Raw HID 0xFC op 0x42: owner (native or mouse) and status, for `tools/multitouch/ptp_owner.py`; action 2 holds Velocity Scroll with no key, for `tools/multitouch/velocity_hold.py` |
| `SVAL_PERF_STATS=yes` | dev builds only | Timing counters over raw HID 0xFC op 0x43, for `tools/multitouch/perf.py`; with or without the touchpad. They include the slowest scan gap split into exclusive parts: matrix row reads, the split exchange of every scan, the pad driver call, the other pad's split fetch, USB IN sends, the counters' own split reads, and everything else |

Release builds refuse `SVAL_TOUCH_DEV` and `SVAL_PERF_STATS`, and
`check_release_elf.py` rejects their symbols. Neither 0xFC command is a
released protocol.

## Tests

- `util/test_native_ptp.sh`: `ptp.c`, `multitouch.c` with `ptp.c` (split link,
  hints, per-hand gating, updates), `pad_schedule.h` against a simulated chip
  and main loop, the descriptor (cross-checked with hid-tools when installed)
  and every report the C tests sent, decoded with it.
- `make test:pointing/azoteq_multitouch`: the driver's contact read.
- `tools/multitouch/ptp_desc_check.py ELF`: the descriptor and interface layout
  inside a built image.

## Measured

On a keyless test board with a pad on each half: the auto build with the
blank keymap, the real matrix scan running on both halves (unpaced, turbo 0,
90/90 us). A keyless board reads its unconnected column inputs as four keys
held, so the scan settles into one unchanging frame; key events themselves
were not exercised.

**Idle**, two 30 s windows each, with and without the touchpad (the same
commit, `SVAL_NATIVE_PTP=no`):

| | Without the touchpad | With it |
|---|---|---|
| USB half: matrix scans per second | 573, 571 | 483, 484 |
| USB half: longest scan gap | 3.3, 3.2 ms | 5.3, 5.2 ms (3 to 4 gaps a second over 4 ms) |
| Other half: matrix scans per second | 762, 772 | 750, 751 |
| Other half: longest scan gap | 6.2, 5.6 ms | 2.6, 2.6 ms |
| Pad reads per second, each half | 96 to 99 | 123 |
| Pad read | 0.64 ms USB half, 0.87 to 0.98 ms other half (100 kHz, 10 bytes, with the end of session) | 0.59 ms USB half, 0.55 ms other half (400 kHz, 10 bytes, the read alone) |
| Other pad fetched over the split link | | 4 a second, 0.81 ms each |

On the USB half the touchpad takes 11% of the time in the pad driver call
(read, end of session and the sample) and 4% in its task. Its slowest idle
gap is one loop pass with a pad read held by the chip (2.5 ms), the matrix
(1.0 ms) and the 4-a-second split fetch (0.8 ms).

**With fingers**, by hand on the USB half, 10 s each, on the build before reads
were sized to the contacts (every slot read while a finger was down):

| | Idle | One finger | Two | Three |
|---|---|---|---|---|
| Pad read | 0.6 ms | 1.65 ms | 2.6 ms | 4.5 ms |
| Matrix scans per second | 483 | 330 | 300 | 252 |

The longest scan gap was up to 9 ms, and once 24.6 ms with one finger on each
pad, with no failed pad read or split fetch.

## Not covered yet

- Linux on hardware.
- Timing while typing (needs a keyed board).
- A pad that holds the I2C clock line low (the back-off is host-tested only).
