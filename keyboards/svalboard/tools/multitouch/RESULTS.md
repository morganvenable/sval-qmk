# Prototype validation — 2026-10-12

## Latest: guided characterization and manipulation prototypes — 2026-10-13

The current firmware is `prototype-characterize.uf2`, SHA-256
`1137c4377300155da36a1a781893f4cfde30c5dd8be73f71e78ab6ebc373f2ad`,
built on `cc6188ee23` with `:blank`, `SVAL_TEST_WIPE=yes`, 400 kHz I²C,
30 ms contact-read timeout and one complete frame per 64-byte HID report.
It remains installed on the authorized test board. Historical builds below are
retained for comparison; they are not the currently installed image.

Before characterization, the revised timeout/transport build captured 4,106
frames over 45 seconds, including 197 five-contact frames, with zero acquisition
errors, queue drops, sequence gaps or incomplete assemblies (`hands-on-fix-02`).
The previous 10 ms timeout build had eliminated transport fragmentation losses
but still counted 230 read timeouts (`hands-on-fix-01`).

The user then completed eight guided phases, each approximately four seconds
after one second of settling, with runtime setting write/readback. All eight had
zero I²C errors, zero sequence gaps and no configuration retries.

| Held fingers | Maximum allowed | Requested ms | Mean sensor cycle ms | Mean blocking acquisition ms | Matching contacts |
|---|---:|---:|---:|---:|---:|
| 1 | 1 | 10 | 10.30 | 1.42 | 100% |
| 1 | 2 | 10 | 10.30 | 1.41 | 100% |
| 1 | 3 | 10 | 10.29 | 1.49 | 100% |
| 1 | 5 | 10 | 10.27 | 1.58 | 100% |
| 3 | 3 | 10 | 11.88 | 10.16 | 100% |
| 3 | 5 | 10 | 12.06 | 10.20 | 99.1% |
| 3 | 5 | 20 | 20.69 | 1.31 | 100% |
| 3 | 5 | 5 | 12.05 | 10.64 | 87.2% |

The user observed occasional loss of one of three contacts. The 5 ms phase had
279 three-contact frames and 41 two-contact frames. This was detection loss with
continuous reports, not a transport gap. It confounds a strict three-finger timing
comparison. No material speed improvement from lowering the allowed touch count
was observed in this short, single-run sweep. The sensor cycle includes host
communication time, so ~12 ms is not established as a silicon processing ceiling.
RR_MISSED was set for every measured sample in the three-finger 5/10 ms phases;
that flag describes missed sensor reporting deadlines, not missing USB reports.
At 20 ms, mean blocking acquisition fell to 1.31 ms. Readiness-driven/asynchronous
acquisition is the next scheduling investigation; long synchronous reads still
delay QMK's main loop even when they complete without errors.

Evidence: `characterize-physical/*.json[l]`, `characterization.html` and
`characterization.png` in the local evidence directory. An earlier unattended
configuration attempt failed transiently; bounded retry was added and the repeat
passed. Physical phases needed no retries. Lease-expiry restoration of the new
settings is implemented but has not received a dedicated hardware fault test.

The two-hand simulation uses the same Python engines exercised by the tests:
clutched one-contact-per-hand pan/zoom/rotation, conservative three-finger swipe
recognition, and a secondary relative-ball adapter. Contact loss, identity changes,
gaps and stale input cancel manipulation until reclutch. There is no physical
second pad, live two-pad routing, or automatic shortcut injection in this lab.
Windows Tk startup/preset/exit smoke test passed. All 31 host unit tests passed,
including 11 new gesture/ball tests. The existing Keybard single-pad integration
remains separate from the new simulation; its UI does not yet expose these controls.

## Historical initial validation

Firmware base: `c4226bb74f`; Keybard base: `28401de`, with existing local changes
preserved. Target: `svalboard/azoteq/right:sval SVAL_AZOTEQ_MULTITOUCH=yes`.
Test board: `sval:E4655015C33E122F`, RP2040 B2, 16 MiB flash. Other connected boards
were not flashed. Windows build 26200, x64 Python 3.13.

## Passed

| Check | Observed result |
|---|---|
| Windows touchpad backend | Required exports present; synthetic device creation/destruction succeeds |
| Actual Windows event delivery | 84 injected frames across two phases; scroll events and `PT_TOUCHPAD` pointer events received by the registered test window |
| Fixed firmware idle stream | 3,844 frames / 40.004 s, zero sequence gaps, incomplete frames, I²C errors, or firmware queue drops |
| Windows relay session at idle | 3,844 frames / 40.001 s, ownership active throughout, zero gaps/errors/drops; one initial epoch synchronization, no further cancellations |
| Sample timing | Median acquisition interval 10 ms; maximum 30 ms |
| Host receive timing | Median 10.002 ms; p99 27.970 ms; maximum 35.032 ms (interarrival, **not end-to-end latency**) |
| Existing Keybard reader + stream | Complete profile loaded in 1.634 s, 25 layer reads succeeded; 360 contact frames processed during the session |
| Ownership watchdog | Requested state 2 became active state 3 at all-up; without renewal, state returned to 0 by the check 750 ms after start |
| Python prototype tests | 16 passed: reassembly, sparse slots, sequence/epoch loss, stale input, dispatcher routing, contact replacement/release and device cancellation |
| Driver prototype tests | 4 passed: coherent full read, failed read, failed session close, fixed coordinate domain |
| Legacy driver regression | 2 existing tests passed |
| Keybard protocol regression | 13 existing tests passed |

The initial hardware run flagged every frame as reset. The sensor's `SHOW_RESET`
bit is latched; the existing mouse driver never needed to acknowledge it. The
prototype now sends `ACK_RESET` to System Control 0 (`0x0431`) during initialization.
After the fix, the reset flags and associated partial-frame discontinuities ceased.
An unexpected later sensor reset remains fail-closed until firmware reinitializes.

## Physical touch session — 2026-10-13

The user supplied actual touches in a 45.009-second observation session on the
same test board. `hands-on-01.jsonl` and its summary are retained with the local
evidence. All five contact slots were observed simultaneously.

| Simultaneous contacts | Received frames |
|---|---:|
| 0 | 1,594 |
| 1 | 626 |
| 2 | 478 |
| 3 | 359 |
| 4 | 451 |
| 5 | 214 |

This run also exposed failures absent at idle: 308 firmware acquisition errors,
25 firmware queue overflows, 320 received sequence discontinuities and 252
incomplete assemblies, out of 3,722 complete received frames. These categories
overlap; they must not be added together as a lost-frame total. Five-contact
acquisition is demonstrated, but reliable movement streaming is **not** established.
Windows relay was not enabled for this session. Investigate acquisition failures
and USB service under active mouse output before judging real gesture behavior.

The user subsequently identified an important confounder: this is a bare MCU,
and the `:sval` keymap generated unwanted keyboard activity. Future builds must
use `:blank`. These error counts describe the observed run; they do not isolate
a sensor or transport defect. Repeat under the blank keymap before assigning cause.
`touch-replay.html` and `touch-replay.png` in the local evidence directory visualize
the recording; the HTML preserves all received frames and marks discontinuities.

The verified prototype UF2 listed below was reinstalled and **left on the board at
the user's explicit request**. Do not automatically restore the original firmware
after subsequent tests on this board. The original verified backup remains intact.

## Limits of the evidence

### Frame-loss fixes — 2026-10-13

After the user confirmed the target identity and authorized further tests, two
physical passes isolated and resolved the observed failures in the final pass:

| Capture | Change | Frames | Acquisition errors | Queue overflows | Sequence gaps | Incomplete frames |
|---|---|---:|---:|---:|---:|---:|
| `hands-on-wipe-02` | Prior blank/wipe baseline | 3,819 | 219 | 28 | 207 | 177 |
| `hands-on-fix-01` | 400 kHz I²C, one complete USB report/frame, bounded queue drain | 4,245 | 230 | 0 | 0 | 0 |
| `hands-on-fix-02` | Additionally, 30 ms contact-read deadline | 4,106 | 0 | 0 | 0 | 0 |

Each run lasted approximately 45 seconds and included one through five physical
contacts. New acquisition counters classified **all 230 errors in the intermediate
pass as read timeouts**, with zero other read errors or session-close failures.
The final pass completed acquisitions taking up to **13 ms**, exceeding the former
10 ms deadline. The new 30 ms deadline is a maximum wait, not a fixed delay.
Final contact-count frequencies 0–5: 2,605 / 247 / 264 / 508 / 285 / 197. All 4,106
received frames were valid; the epoch stayed constant. Sample intervals had a
10 ms median and 31 ms maximum. This proves a clean measured pass, not a sustained
100 Hz rate under every condition or a long-duration reliability guarantee.

The final firmware is left installed on the authorized test board. Its UF2 is
`prototype-timeout-fix.uf2`, SHA-256
`21de792b1fe71b1b7342f3317398922a85f25ae518b3e1cc5ed62ec8f3b2c035`.
The intermediate UF2 was `prototype-loss-fix.uf2`, SHA-256
`bf184064f201d90895f25578c6f2265879b08b5933cb446fd108629a010db595`.
The final replay/preview are `touch-replay-clean.html` and `touch-replay-clean.png`.
Twenty host tests and four acquisition driver tests passed. The host suite now
checks complete 64-byte frames, session isolation and dispatcher preservation of
the entire compact report. Other connected boards were not selected for operations.

### Latest blank/wipe build — 2026-10-13

At the user's direction, the prototype was ported onto `cc6188ee23`
(`feat/hid64-pointing-auto`) in `.local/multitouch-latest` and built with
`svalboard/azoteq/right:blank SVAL_AZOTEQ_MULTITOUCH=yes SVAL_TEST_WIPE=yes`.
This uses the existing wipe mechanism, not a custom key-lookup suppression patch.
The newer base uses 64-byte physical Raw HID reports; the prototype now supports
padding its existing logical packets to that size. Host tests: 17 passed.

The firmware was flashed and verified on the same exact test board. After boot,
a complete readback verified **all 1,920 keymap bytes across 16 layers were zero**.
The subsequent 45.004-second capture delivered 4,324 frames with no errors, queue
drops, sequence gaps or incomplete assemblies. It contained zero touches, so the
physical movement comparison against the earlier nonblank run is still pending.

A subsequent requested physical repeat, `hands-on-wipe-02`, captured 3,819 complete
frames over 45.001 seconds, including 156 valid five-contact frames between
26.098 and 29.241 seconds. Valid frame counts by contact count 0–5 were
1,885 / 545 / 583 / 278 / 133 / 156. Firmware counters increased by 219 acquisition
errors and 28 queue overflows; the host observed 207 sequence discontinuities and
177 incomplete assemblies. Of received complete frames, 239 carried fault flags.
Errors therefore persist under the verified blank/wipe build: keyboard spam is
not a sufficient explanation. Different finger movements and firmware bases mean
these runs are not a controlled quantitative performance comparison.
The new interactive replay and preview are `touch-replay-wipe-02.html` and
`touch-replay-wipe-02.png`; both explicitly identify the blank/wipe capture.

The latest firmware is **left installed**, per the user's instruction. Its UF2 is
`prototype-latest-wipe.uf2`, SHA-256
`a6ae1ce60c4c3507e7e490c8b9cebb3494e48cf0272ca0ee0fa1a0f976593592`.
Evidence: `check-blank.json`, `hands-on-wipe-01.jsonl`, its summary, build log and
`latest-base-tracked.patch` in the persistent local evidence directory. Untracked
prototype source files are retained in both the main and isolated checkouts.

The earlier idle captures contained zero fingers. The later physical session
demonstrated one through five simultaneous contacts, with the failures above.
Windows event delivery was generated from synthetic trajectories. Full physical
pad-to-Windows gesture behavior, tracking accuracy, cancellation UX, and
palm/reati behavior still require hands-on validation.

The real Keybard protocol code was exercised on Windows with the physical board.
The full Qt overlay was not launched on Windows. A broader Linux Qt test attempt
failed in the environment's QtWebEngine sandbox/EGL setup; no full GUI-suite pass
is claimed. The targeted protocol suite passed.

No split contact transport, Linux backend, native USB PTP descriptor, suspend/resume
validation, physical disconnect-under-touch test, or long-duration soak is included.

## Reproducibility and recovery

Persistent local evidence (not committed): `.local/multitouch-2026-10-12/` in the
firmware checkout. It contains the verified original full flash, hash manifest,
UF2s, build logs, capture JSONL, Windows event records and Keybard/lease results.

- Original 16 MiB flash SHA-256:
  `ffdc638c3030834fc0361ca9c7104ede31bb96618b7d7ca3ad27a32330a2feab`
- Fixed prototype UF2 SHA-256:
  `7f6832b28fcbd02b864d90360a2bf758a4f23ac604cc5e9e0d462a6c6d00c35e`
- Initial restoration: the original full 16 MiB image was restored and read-back verified
  successfully. The board rebooted and its exact application serial reappeared.
  The subsequent physical session reinstalled the prototype; it is now the firmware
  left on the board, per the user's explicit instruction above.

## 2026-10-13: unified pair invalid-frame diagnosis and recovery

An idle 15-second baseline on firmware 2302 recorded 2,957 frames and 29 sensor
acquisition-invalid frames (16 left, 13 right). The unified host counted each
invalid frame twice: its epoch change as a pad restart, then its invalid flag.
Replay therefore showed 58 warnings despite no active gesture. No host heartbeat
or synchronization faults appeared in that baseline. USB queue drops were zero.

The RP2040 ChibiOS I2C interrupt handler discarded address-NACK causes before QMK
could inspect them. The prototype now preserves the abort reason and retries one
pure 7-bit address NACK after 200 microseconds. It does not retry timeouts,
arbitration loss, data NACKs, other bus faults, or end-session failures. The raw
abort value 0x00800001 is address NACK plus one flushed FIFO entry; FIFO count bits
are not error causes. This is consistent with polling a closed communication
window; the capture does not establish which internal sensor mode caused it.
Azoteq documents first-address NACK and retry in its IQS5xx B000 datasheet §8.8.2.

Firmware 2305 on both permanent blank test boards produced 9,000 valid frames in
45.01 seconds: exactly 4,500 per hand, no sequence gaps, no incomplete packets,
no invalid flags, and no USB queue drops. On the USB-connected right MCU, 3,604
additional address-NACK attempts all recovered on the single retry. No final
read/end errors or timeouts occurred; lifetime maximum read duration was 6 ms.
Remote retry counters are not transported, but all 4,500 remote frames were valid.
Replay through the unified model had zero faults and zero cancellations.

The UI now distinguishes per-hand acquisition faults from stale split snapshots,
counts canceled active gestures separately from idle faults, and logs raw frames
with receipt timestamps for replay. Safety thresholds and all-up rearming remain.

Validation: seven sensor-driver tests, 45 host tests, the compiled split fixture,
the actual HAL handler with abort-register fixtures, both flash RAM checks,
matching split transaction tables, and hash-verified pair update. This hardware
comparison was idle (zero contacts); sustained physical gestures still need a
fresh user exercise. The 45-second app identity refresh still deliberately rearms.

Local evidence: `.local/touchpad-reliability-2026-10-13/` in the main workspace,
including baseline and all intermediate captures, final `after-2305.summary.json`,
raw frames, replay totals, build logs, pair-update logs, and firmware hashes.
