# Svalboard in-firmware updater

The updater lets a running Svalboard half replace its own firmware over USB,
without BOOTSEL or a UF2 copy. It is compiled in only with `SVAL_UPDATER=yes`
and is off in every default build. This page describes what is built today
(milestone M1: one half, over its own USB). The design and its decisions are in
[updater-plan.md](updater-plan.md); code is in `keyboards/svalboard/updater/`
(below, `kb/` is `keyboards/svalboard/`).

In short: the host sends a signed manifest and the user approves it on the
keyboard. The half then stages the raw image in spare flash, checks its
signature, hash and structure, and on COMMIT copies it over the running
firmware from RAM. Before the first erase it makes the old image unbootable,
and it writes the new image's first page last. A power cut at any point
leaves either the new image or a board that comes up in BOOTSEL. It never
leaves a half-written image that the boot ROM would run.

## Builds

| Flag | Effect |
|---|---|
| `SVAL_UPDATER=yes` | The updater: VIA channel `0x55`, staging slot, commit routine, LED states. On its own it accepts no image until M3 adds the release keys |
| `SVAL_UPDATE_TEST_KEY=yes` | Also accepts images signed with the TEST-ONLY key (`key_id 0`), whose seed is in the repository (`kb/tools/sval_update_TEST_ONLY.key`). Test boards only; the build prints a warning |
| `SVAL_UPDATE_TEST_HOOKS=yes` | Adds the `TEST_HALT` op and the commit halt points (hardware tests only). Implies `SVAL_UPDATE_TEST_KEY` |
| `SVAL_UPDATE_RELEASE=yes` | Release updater build: DIAGNOSTIC images and `key_id 0` refused. Cannot be combined with the test key or test hooks |

`SVAL_UPDATER` cannot be combined with `SVAL_KEYTEST` (keytest injects key
events, so a host could fake the confirmation chord). Never ship a build with
the test key: it accepts firmware from anyone who can also make the chord.
The M1 hardware tests use `SVAL_UPDATE_TEST_KEY=yes` (or the test hooks):

```
make svalboard/trackball/pmw3389/right:sval SVAL_UPDATER=yes SVAL_UPDATE_TEST_KEY=yes
python3 -I keyboards/svalboard/tools/check_ram_funcs.py .build/svalboard_trackball_pmw3389_right_sval.elf
```

## Flash map

The RP2040 boots from a 16 MiB W25Q128-class die. Offsets are flash offsets (the
XIP address is `0x10000000` plus the offset).

| Flash offset | Contents |
|---|---|
| `0x000000`-`0x0000FF` | Page 0: boot2. The boot ROM checks a CRC-32/MPEG-2 over bytes 0-251 against the word at 252; if it fails, it boots into USB (BOOTSEL) |
| `0x000100`-`0x15FFFF` | The rest of the firmware (vector table at `0x100`). `kb/flash_reservation.ld` keeps every image below `0x160000` (`SVAL_UPDATE_MAX_IMAGE`) |
| `0x160000`-`0x25FFFF` | Settings: QMK wear leveling, two copies of `0x80000` |
| `0x260000`-`0x7FFFFF` | Free |
| `0x800000`-`0x95FFFF` | Staging slot (`SVAL_UPDATE_BASE`, `SVAL_UPDATE_SIZE`) |
| `0x960000`-`0xFFDFFF` | Free |
| `0xFFE000`-`0xFFFFFF` | Board identity, two 4 KiB sectors |

Staging erases and programs only inside the slot. The commit erases and programs only inside
`[0, round_up(image_len, 64 KiB))`, which is at most `[0, 0x160000)`. Both limits
are checked again in the RAM routine just before each boot ROM call. Static asserts
in `update_flash.c` and `update_commit.c` pin the layout. Nothing writes the
settings or identity regions. On a die other than 16 MiB, the updater reports
UNAVAILABLE and touches nothing.

## Protocol

The updater is a keyboard-local VIA custom-value channel (`0x55`, `'U'`) inside
the Sval client wrapper:

```
[0xDD][client_id:4][0xFE] [id_custom_set_value 0x07 | get 0x08][0x55][op][value_data:23]
```

The reply is the same packet with `value_data` rewritten. `value_data[0]` is
always a status. Every request starts with `[hand]`: this half (0 left, 1 right),
or `0xFF` for INFO only. Any other hand gets UNSUPPORTED, because M1 has no
split relay. Every op except INFO needs the client wrapper; an unwrapped packet
gets INVALID and changes nothing. Multi-byte fields are little-endian. The
byte layouts are in the comment at the top of `kb/updater/updater.c`. In short:

| Op | Request after `[hand]` | Reply after `[status]` / rule |
|---|---|---|
| `0x00 INFO` | - | state, protocol, slot base/size and max image (4 KiB units), JEDEC ID, pointing ID, hand, fw version, storage format, security epoch, flags (16 MiB die, settings writes failing, release build, test hooks, test key accepted, the other half's version differs), last error |
| `0x01 MANIFEST` | off, n ≤ 20, bytes | bytes filled, of 172 (108 B manifest then 64 B Ed25519 signature). In order; offset 0 restarts. Ties the load to this client |
| `0x02 ARM` | - | nonce u32 and the first 4 bytes of SHA-512(manifest), which the host shows. Checks the manifest fields, binds the session to client ID + nonce, starts the 30 s chord window. BUSY, with nothing changed, while a Scan Lab sweep runs |
| `0x03 BEGIN` | nonce | ACCEPTED once the chord is made: signature check, then the erase, one 4 KiB sector per main-loop pass |
| `0x04 CHUNK` | off u24, n ≤ 18, bytes | next offset. Strictly in order; an earlier chunk is acknowledged and not written again |
| `0x05 END` | nonce | ACCEPTED: SHA-512, structure checks and CRC of the staged image, 2 KiB per pass |
| `0x06 STATUS` | - | state, bytes staged, erase progress, last error, timings, CRC low 16 bits once VERIFIED, flags, the other half by split presence (see below) |
| `0x07 COMMIT` | nonce, CRC low 16 | ACCEPTED; the commit runs from housekeeping 100 ms later. The CRC (CRC-32/MPEG-2 over image bytes `[0x100, len)`) must match the verified image |
| `0x08 ABORT` | nonce | From the session, or from anyone while ERROR is latched. Never erases |
| `0x09 REBIND` | nonce | Moves the session to the sender's client ID (Keybard renews its ID every 50 s); the old ID is refused after it |
| `0x0A TEST_HALT` | nonce, point, fed | Test-hooks builds only, in VERIFIED: where the commit stops (see Test hooks). INVALID elsewhere |
| `0x0B DIAG` | clear | Main's stack: bytes never used since boot and its size; the longest `updater_task()` pass in ms and the state it began in. `clear` 1 restarts the pass record. For M1 #13 |

States: `IDLE 0, MANIFEST_LOADING 1, CONFIRM_WAIT 2, VERIFYING_MANIFEST 3,
ERASING 4, RECEIVING 5, VERIFYING_IMAGE 6, VERIFIED 7, COMMITTING 8, ERROR 9`.
Status codes: `OK 0, INVALID 1, UNAVAILABLE 2, FLASH_ERR 3, ACCEPTED 4, BUSY 5,
NOT_CONFIRMED 6, WRONG_HW 7, BAD_SIG 8, BAD_HASH 9, BAD_IMAGE 10, EPOCH 11,
OUT_OF_ORDER 12, OVERRUN 13, TOO_LARGE 14, TIMEOUT 15, OTHER_CLIENT 16,
UNSUPPORTED 17, STORAGE 18` (`kb/updater/update_manifest.h`). A refused request
(another client, a wrong nonce, a request out of order) changes nothing. A
failed update (a check fails, a flash error, OVERRUN, a timeout, a refused
commit) latches ERROR until ABORT. A session with no op for 30 s times out.

**What the session binding protects against.** The client ID and nonce keep
well-behaved clients (two Keybard tabs, a stray tool) from crossing a session.
They do not stop a hostile one (R10: the client ID is not a secret). The client
ID is in every wrapped reply and the nonce in ARM's, and raw HID replies reach
every open handle, so another process with the device open can REBIND the
session to itself and then stall it, ABORT it, or choose when to COMMIT it. It
cannot change what is installed: the user confirmed the manifest hash with the
chord, and the signature and SHA-512 bind the image to that manifest. The M1 #6
rows "a second client ID", "ABORT from another client" and "REBIND with the
wrong nonce" therefore test the binding against accidental cross-talk only.

**Confirmation.** After ARM, hold Index South + Middle South for 1 s on the half
being updated. On the right half that is matrix `[6,0]+[7,0]` (`M` and `,` on
the default keymap); on the left it is `[1,0]+[2,0]` (`V` and `C`). The chord is
read from the scanned matrix, never from keycodes, and both keys are hidden from
QMK while the updater waits. The host can set the matrix scan timing (the saved
pre/post waits, and Scan Lab sweeps), and a pre-wait that is too short can
misread keys. So while the chord is awaited every row is scanned with at least
Scan Lab's safe timing (500 µs before and after, `SVAL_UPDATE_CHORD_PREWAIT_US`
/ `POSTWAIT_US`), ARM is refused while a sweep runs, and Scan Lab refuses to
start a sweep or a probe while the updater is active.

**LEDs.** Blue blink: make the chord. White double flash: chord recognised. Solid
blue: approved, waiting for BEGIN. Rainbow: verifying, erasing, receiving.
Magenta: writing. Red at 1 Hz: error, until ABORT.

**Manifest checks.** Magic `SVUP`, version 1, reserved fields 0, known flags;
the key is accepted by this build; the hand and pointing device match this build;
the security epoch and storage format are at or above this device's floors;
`image_len` is between `0x200` and `SVAL_UPDATE_MAX_IMAGE` and a multiple of 256.
On the staged image: SHA-512 equals the manifest's, the boot2 CRC of page 0 is valid,
the initial SP is in `[0x20000000, 0x20042000]`, and the reset vector is odd and
inside the image. `kb/tools/make_update.py` refuses to build an image that would
fail any of these.

## The commit and the reset mechanism

`update_commit.c`. Step 0 runs from flash with interrupts on. Steps 1-7 are the
`commit_ram_*` functions, which run from RAM (`.time_critical`) with PRIMASK set.

0. **Re-check.** Refuse, with nothing changed, if settings writes are failing
   (`wear_leveling_write_failed()`), the die is not 16 MiB, the manifest fields
   no longer pass, the slot's SHA-512 or body CRC differs from what was verified,
   the head checks fail, the running boot2 (page 0) fails its CRC, a boot ROM flash
   function is not a ROM Thumb address, or the watchdog tick is not running. Take
   the commit's own copy of the running boot2 from page 0; it is never refilled.
   Look up the ROM flash functions. Show magenta, latch it with two WS2812
   flushes, and wait for that transfer to finish.
1. **Arm.** Set PRIMASK. If any of the 12 DMA channels is busy, clear PRIMASK
   and refuse with BUSY. Point VTOR at a 48-slot RAM vector table whose handlers
   are all the RAM reset routine, so any fault resets the board. Arm the watchdog,
   in this order: `PSM_WDSEL = 0x1FFFC` (everything but the oscillators; at its
   reset value 0, a watchdog reset would reset nothing), clear scratch4,
   `LOAD = 8 s × 2` (the counter drops 2 per µs), clear the pause-on-debug bits,
   set ENABLE.
2. **Invalidate.** Program page 0 to all zeros and read it back through the
   uncached alias. A zero page 0 fails the boot ROM's CRC, so from here on a cut
   ends in BOOTSEL. If the read-back is not all zero, reset at once: most of the
   old image is still there.
3. **Erase.** Sector 0, then the other 15 sectors of block 0 one at a time, then
   whole 64 KiB blocks up to `round_up(image_len, 64 KiB)`.
4. **Copy.** For each 4 KiB sector: slot → the commit's dedicated 4 KiB RAM
   buffer with XIP on, exit XIP, program (sector 0 from page 1), flush the cache,
   and re-enter XIP through the boot2 copy.
5. **Verify.** CRC-32/MPEG-2 over `[0x100, image_len)` read back uncached,
   against the CRC from VERIFYING_IMAGE. On a mismatch, every sector that
   differs from the slot is erased and programmed again, once. If the CRC still
   fails, page 0 stays erased and the board resets into BOOTSEL.
6. **Page 0.** Read the image's page 0 from the slot, check it against the CRC
   taken in step 0, program it, and read it back. If the read-back differs, page
   0 is programmed to zeros, so the boot ROM goes to BOOTSEL and never runs a
   damaged boot2.
7. **Reset.** Zero the sector buffer. Zero every word of SRAM0-3 that equals the
   double-tap magic `0xCAFEB0BA`, so the new image's noinit magic word cannot
   land on stale bytes and send it to BOOTSEL. Make sure PSM_WDSEL is set, clear
   scratch4 (so the boot ROM boots from flash and not to an address), and write
   the watchdog trigger.

The watchdog is fed after every boot ROM call. The worst single operation is a
64 KiB block erase, 2 s at the datasheet maximum, so the 8 s period leaves room.
Nothing in steps 1-7 calls flash. `kb/tools/check_ram_funcs.py` checks the linked
ELF: every branch in the RAM functions stays inside the RAM set or goes to the boot
ROM, every `blx rN` takes its target from the ROM pointer table or the boot2 copy,
there are no jump tables, no literals in flash, no double-tap magic, and the stack
use is static and at most 512 B. Run it on every `SVAL_UPDATER` build.

## Recovery

| Situation | What happens | Recovery |
|---|---|---|
| Cut or unplug before COMMIT (any staging state) | The firmware area was never touched; the old image boots. The slot is ignored and erased by the next BEGIN | None |
| Cut during the commit, before page 0 is written | Page 0 is zero or erased: the boot ROM boots into BOOTSEL and the `RPI-RP2` drive appears | Copy a UF2: `keyboards/svalboard/tools/flash.sh <image.uf2>`, or drag it onto `RPI-RP2` |
| Cut after page 0 is written | The new image boots | None |
| Fault or hang inside the commit | The RAM vector table or the watchdog (8 s) resets the board, into BOOTSEL until page 0 is written | As above |
| Any time | Hold BOOTSEL while plugging in, or tap the reset button twice within 500 ms | As above |

Settings (`0x160000`-`0x25FFFF`) and identity (`0xFFE000`-`0xFFFFFF`) are outside
everything the commit writes, so they survive all of these. The hardware tests
compare picotool dumps of both regions before and after.

## Test hooks

On a `SVAL_UPDATE_TEST_HOOKS=yes` build, `sval_update.py update FILE.svup --halt
POINT [--halt-unfed]` sends TEST_HALT after VERIFIED, then COMMIT. The commit
stops for good at:

| Point | Where | Page 0 afterwards (`picotool save -r 0x10000000 0x10000100`) | Expected (M1 #8 / #9) |
|---|---|---|---|
| `invalidated` (1) | N = -1: page 0 zeroed and read back, nothing erased | all `0x00` | BOOTSEL |
| `first-erase` (2) | N = 0: sector 0 erased | all `0xFF` | BOOTSEL |
| `mid-program` (3) | after programming the middle sector | all `0xFF` | BOOTSEL |
| `last-sector` (4) | every sector programmed, page 0 still erased | all `0xFF` | BOOTSEL |
| `page0` (5) | page 0 written and read back, before the reset | the new image's boot2 | the new image boots |
| `fault` (6) | a HardFault (`udf`) straight after the invalidation (M1 #10) | all `0x00` | BOOTSEL in well under 1 s |
| `fault-erased` (7) | a HardFault after sector 0 is erased, so the flash vector table is gone (M1 #10, R3) | all `0xFF` | BOOTSEL in well under 1 s |

In every case up to `last-sector` the boot2 CRC of page 0 is invalid and the
board comes up as `RPI-RP2`. Page 0 is all zeros only at N = -1: step 3 erases
sector 0 straight after the invalidation, so from N = 0 on it reads `0xFF`.

Before it stops, a halt clears RAM as step 7 does (the sector buffer and every
double-tap magic word), so an unfed halt's watchdog reset starts the next image
from the same RAM state as a real commit. It then spins with interrupts off and
feeds the watchdog, so it hangs until it is unplugged (M1 #8). With
`--halt-unfed` it stops feeding, and the watchdog resets it within 8 s (M1 #9,
#15). Steps 1-7's own reset path (the watchdog trigger in step 7) is exercised
only by a full commit (M1 #4, #14).

**Timing is the M1 #10 result.** The fault points reset through the RAM vector
table at once. If the RAM table were not in use, the fault would land in the old
firmware's handler (an endless loop, at `fault`) or lock up on the erased table
(at `fault-erased`), and only the watchdog would reset the board, about 8 s
later, into the same BOOTSEL. So a reset into `RPI-RP2` well under 1 s after the
LEDs turn magenta is a pass, and one after about 8 s is a failure.

## Hardware test notes (M1)

- **#6, protocol rows:** `sval_update.py reject GOOD.svup` sends each malformed
  or misdirected request (unwrapped packets, a second client ID, wrong nonces,
  BEGIN before the chord, an out-of-order CHUNK, END too early, ABORT and REBIND
  from the wrong client, COMMIT with the wrong CRC, an overrun) and checks the
  status and the state after each. It needs the chord twice and never commits.
  The 30 s stall is `update GOOD.svup --delay 31` (TIMEOUT).
- **#6, image rows:** `.svup` files from `make_update.py --unsafe-allow-bad-image`
  (test key only), sent with `update`. "Each gives its own status" means its
  expected status: a bad SHA-512 latches BAD_HASH at VERIFYING_IMAGE, and a
  COMMIT with the wrong CRC is refused with BAD_HASH while the session stays
  VERIFIED; the security epoch gives EPOCH and the storage format STORAGE.
- **#6, epoch below:** the device floor is 0 by default, so build the test board
  with `EXTRAFLAGS=-DSVAL_UPDATE_SECURITY_EPOCH=1` and send an image made with
  `--epoch 0`.
- **#6, `key_id 0` on a release-flagged build:** until M3 a release build has
  no keys at all, so every `key_id` gets BAD_SIG, and a DIAGNOSTIC image is
  refused for its key before its flag is looked at. On hardware this row only
  shows that a release build accepts nothing; record it as such and repeat it
  in M3. The host tests check the release rules themselves (`test_manifest_check`:
  the test key, a RELEASE image with the test key, and DIAGNOSTIC with a release
  key).
- **#13:** `sval_update.py diag` after a full update gives main's stack never
  used since boot (crt0 fills it with `0x55555555`; the count is a little
  optimistic if a used word holds the pattern) and the longest updater pass in
  ms with its state. The host tests' 50 ms check counts only simulated flash
  time; the CPU-bound passes (signature, SHA-512 slices, erased checks, commit
  step 0) are measured here. `diag --clear` restarts the pass record.
- **#16:** `update GOOD.svup --rebind-every 5 --min-rebinds 3`. The default 50 s
  renewal may never fire during a fast transfer; `--min-rebinds` fails the run
  before COMMIT if fewer renewals happened, and the summary prints the count.

## The split link (M2a, stage 1)

M2 updates the half without USB through the half with USB. Stage 1 builds
the link layer; the relay, the slave's mailbox and programming come in
stage 2. All of it is in `SVAL_UPDATER` builds only: a default build is
unchanged byte for byte.

**One release on both halves (V).** An `SVAL_UPDATER` build adds the
`KEYBOARD_UPDATE` split transaction, so its split table has one entry more
than a default build's. Both halves fold the transaction count into every
split handshake, so an `SVAL_UPDATER` half cannot talk to a default-build half
at all: flash both halves with `SVAL_UPDATER` builds of the same commit.
`kb/tools/check_split_tables.py` checks that every left/right pairing of a set
of builds has the same table (P1):

```
python3 -I keyboards/svalboard/tools/check_split_tables.py --dir DIR_WITH_ELFS
python3 -I keyboards/svalboard/tools/check_split_tables.py --dump BUILD.elf
```

**Presence with version (D17).** The half without USB answers the existing
500 ms presence ping (`KEYBOARD_SYNC_A`) with 17 bytes: magic `PV`, struct
version 1, `fw_version` u32 (`SVAL_FW_VERSION`, 0 until M3), the first 8 hex
digits of the git hash (0 for `SKIP_GIT` builds), updater protocol, hand,
pointing ID, flags (bit0 dirty tree, bit1 updater active), a reserved 0 and a
CRC-8 (`kb/updater/update_split_wire.h`). The half with USB compares it with
its own build and reports the result in STATUS byte 22 (`value_data[22]`):

| Code | Other half |
|---|---|
| 0 | no answer (or this is the half without USB) |
| 1 | same release: same `fw_version`, git hash and updater protocol, other hand |
| 2 | version differs |
| 3 | built for the same hand |
| 4 | the answer fails magic, version or CRC |

For 2-4, INFO flags bit5 is set and, while the updater is idle, the half with
USB blinks the red error LED (1 Hz) until the halves match (V). The pointing
device may differ between halves (D18).

**The split pause (D15).** `kb/split_pause.c` overrides QMK's weak
`matrix_scan()`. While the half with USB is paused, it skips the whole split
exchange (keys, layers, RGB sync, pointing, the watchdog ping), the presence
ping and the Scan Lab relay; only `KEYBOARD_UPDATE` RPCs cross the link. On
entry the other half's keys are released once and its last pointer report is
dropped; this half keeps typing. On resume the split watchdog is re-armed, so
a half that rebooted meanwhile is pinged again. Only the half with USB can
pause, and only while the link is up. Stage 1 adds the mechanism; nothing
calls it yet.

**`KEYBOARD_UPDATE`.** Frames of at most 32 bytes each way:
`[op][seq][payload][crc16]` and `[status][op][seq][state][payload][crc16]`
(CRC-16/CCITT-FALSE). Ops: `INFO 0, BEGIN 1` (the 172-byte signed manifest in
fragments of up to 26 bytes), `PAGE 2` (image bytes, up to 24 per fragment,
never across a 256-byte page), `STATUS 3, END 4, COMMIT 5` (CRC low 16),
`ABORT 6`. The layouts are in `kb/updater/update_split_wire.h`. The slave
answers a repeated frame (same seq and CRC) from its cache without running it
again, and a PAGE fragment it already holds is acknowledged and not written
again. In stage 1 the slave answers INFO, STATUS and ABORT; BEGIN, PAGE, END
and COMMIT are checked and refused with UNSUPPORTED.

## Tools and tests

- `kb/tools/make_update.py`: UF2 → `.svup` (manifest, signature, raw image). It
  refuses malformed UF2s and images the device would refuse.
- `kb/tools/sval_update.py`: the M1 host tool (`list`, `info`, `status`, `diag`,
  `abort`, `update`, `reject`). Close Keybard first: raw HID replies reach every
  open handle.
- `kb/tools/check_ram_funcs.py BUILD.elf`: the RAM-code check above.
- `kb/tools/check_split_tables.py`: the split-table check across left/right
  pairings (P1), above.
- `util/updater_test/run.sh [UF2 ...]`: host tests under ASan and UBSan. They
  cover `make_update.py`, the manifest, image and crypto checks, the session state
  machine, `sval_update.py` against the C state machine, and the commit routine.
  The commit tests run `update_commit.c`'s real step logic against a mock die and
  mock registers (`tests/sval_updater/update_commit_host.h`). They cut the power
  after every flash operation of staging and of the commit, both cleanly and
  torn (random bytes in the sector, block or pages the operation touched), and
  then check that staging changed only the slot, the commit changed only
  `[0, round_up(len, 64 KiB))`, nothing else changed, every boot ROM call came in
  order (interrupts off, XIP exited; the mock die refuses any other call), and
  page 0 is either invalid or the whole new image is in place.
  `test_split.c` covers the split link: the pause against mocks of QMK's
  matrix and transport (a key held on the other half at pause entry is
  released, the exchange stops, this half keeps typing, resume re-arms the
  split watchdog), the `KEYBOARD_UPDATE` frames (every single-bit error is
  caught), PAGE fragment assembly, presence and the mismatch rules, and the
  slave's handler (retries answered from its cache, 20,000 random frames).
