# Svalboard in-firmware updater

The updater lets a running Svalboard half replace its own firmware over USB,
without BOOTSEL or a UF2 copy. It is compiled in only with `SVAL_UPDATER=yes`
and is off in every default build. This page describes what is built today:
milestone M1 (one half, over its own USB), M2a (the half without USB,
relayed through the half with USB; see "The other half" below) and M3 (release
signing and release CI; see "Release signing" below). The design and its decisions are in
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
| `SVAL_UPDATER=yes` | The updater: VIA channel `0x55`, staging slot, commit routine, LED states. Accepts images signed with either release key (`key_id 1`, `2`; `kb/updater/update_release_keys.h`) |
| `SVAL_UPDATE_TEST_KEY=yes` | Also accepts images signed with the TEST-ONLY key (`key_id 0`), whose seed is in the repository (`kb/tools/sval_update_TEST_ONLY.key`). Test boards only; the build prints a warning. It still accepts the release keys, so a test board can take a signed release image |
| `SVAL_UPDATE_TEST_HOOKS=yes` | Adds the `TEST_HALT` op and the commit halt points (hardware tests only). Implies `SVAL_UPDATE_TEST_KEY` |
| `SVAL_UPDATE_RELEASE=yes` | Release updater build: DIAGNOSTIC images and `key_id 0` refused. Cannot be combined with the test key, test hooks, `SVAL_KEYTEST` or `SVAL_HOST_BOOTLOADER`. Release CI builds every release keymap this way |
| `SVAL_FW_VERSION=N` | The numeric version (D17), reported by INFO and presence. Default: `kb/updater/fw_version.txt` (D32). The older `EXTRAFLAGS=-DSVAL_FW_VERSION=N` still works |
| `SVAL_FW_VERSION_STRING=S` | The version string, at most 16 of `A-Z a-z 0-9 . _ + -` (INFO page 1). Default empty; release CI passes the tag |

Every updater build carries a 28-byte build-info record,
`sval_update_build_info` (`kb/updater/update_keys.h`): magic `SVBI`, which of
the flags above it was built with, the number of release key slots, the
updater protocol, `SVAL_FW_VERSION` and `SVAL_FW_VERSION_STRING`.
`make_update.py` takes the manifest's version from it, its signer refuses an
image whose record is not a clean release build, and the release lint reads
it.

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
the other half (M2, below), or `0xFF` for INFO only. Any other hand gets
UNSUPPORTED. Every op except INFO needs the client wrapper; an unwrapped packet
gets INVALID and changes nothing. Multi-byte fields are little-endian. The
byte layouts are in the comment at the top of `kb/updater/updater.c`. In short:

| Op | Request after `[hand]` | Reply after `[status]` / rule |
|---|---|---|
| `0x00 INFO` | page (0, the default) | state, protocol, slot base/size and max image (4 KiB units), JEDEC ID, pointing ID, hand, fw version, storage format, security epoch, flags (16 MiB die, settings writes failing, release build, test hooks, test key accepted, the other half's version differs, bit6 the release keys are the DRY RUN keys), last error. Page 1 (M3, this half only): `1`, fw version u32, the 16-character version string from the build-info record. Other pages: INVALID |
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
| `0x0C RELAY` | - | M2: the relay to the other half: phase, that half's state and last error, bytes it acknowledged, image length, its erase progress, retries, relay ms, link paused, the relay's error, this half's state |

States: `IDLE 0, MANIFEST_LOADING 1, CONFIRM_WAIT 2, VERIFYING_MANIFEST 3,
ERASING 4, RECEIVING 5, VERIFYING_IMAGE 6, VERIFIED 7, COMMITTING 8, ERROR 9`,
and for a session for the other half `RELAYING 10, RELAYED 11,
SUBSIDE_COMMITTING 12`.
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
- **#6, `key_id 0` on a release-flagged build:** in M1 a release build had no
  keys at all, so this row only showed that it accepted nothing. Since M3 a
  release build has the two release keys: repeat the row on hardware with a
  release build (`SVAL_UPDATE_RELEASE=yes`), a test-key image (BAD_SIG) and a
  `.svup` signed with a release key (accepted). The host tests check the
  release rules themselves (`test_manifest_check`: the test key, a RELEASE
  image with the test key, and DIAGNOSTIC with a release key; `test_release.c`:
  a release build's key set).
- **#13:** `sval_update.py diag` after a full update gives main's stack never
  used since boot (crt0 fills it with `0x55555555`; the count is a little
  optimistic if a used word holds the pattern) and the longest updater pass in
  ms with its state. The host tests' 50 ms check counts only simulated flash
  time; the CPU-bound passes (signature, SHA-512 slices, erased checks, commit
  step 0) are measured here. `diag --clear` restarts the pass record.
- **#16:** `update GOOD.svup --rebind-every 5 --min-rebinds 3`. The default 50 s
  renewal may never fire during a fast transfer; `--min-rebinds` fails the run
  before COMMIT if fewer renewals happened, and the summary prints the count.

## The split link (M2a)

M2 updates the half without USB through the half with USB: stage 1 is the
link layer (below), stage 2 the relay (next section). All of it is in
`SVAL_UPDATER` builds only: a default build is unchanged byte for byte.

**One release on both halves (V).** An `SVAL_UPDATER` build adds the
`KEYBOARD_UPDATE` split transaction, so its split table has one entry more
than a default build's. Both halves fold the transaction count into every
split handshake, so an `SVAL_UPDATER` half cannot talk to a default-build half
at all: flash both halves with `SVAL_UPDATER` builds of the same commit.
`kb/tools/check_split_tables.py` checks that every left/right pairing of a set
of builds has the same table (P1). The keyboard RPC entries of that table
are all zero in the ELF (they are filled at boot), so updater builds also
carry `sval_split_kb_ids` (`kb/updater/update_split.c`): each keyboard RPC's
ID with a tag, which the check compares too (the order of the IDs):

```
python3 -I keyboards/svalboard/tools/check_split_tables.py --dir DIR_WITH_ELFS
python3 -I keyboards/svalboard/tools/check_split_tables.py --dump BUILD.elf
```

**Presence with version (D17).** The half without USB answers the existing
500 ms presence ping (`KEYBOARD_SYNC_A`) with 17 bytes: magic `PV`, struct
version 1, `fw_version` u32 (`SVAL_FW_VERSION`: `kb/updater/fw_version.txt`), the first 8 hex
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
pause, and only while the link is up. The relay pauses the link around the
other half's slow work (below).

**`KEYBOARD_UPDATE`.** Frames of at most 32 bytes each way:
`[op][seq][payload][crc16]` and `[status][op][seq][state][payload][crc16]`
(CRC-16/CCITT-FALSE). Ops: `INFO 0, BEGIN 1` (the 172-byte signed manifest in
fragments of up to 26 bytes), `PAGE 2` (image bytes, up to 24 per fragment,
never across a 256-byte page), `STATUS 3, END 4, COMMIT 5` (CRC low 16),
`ABORT 6`. The layouts are in `kb/updater/update_split_wire.h`. The slave
answers a repeated frame (same seq and CRC) from its cache without running it
again, and a PAGE fragment it already holds is acknowledged and not written
again. A repeated END or COMMIT whose cached answer is gone is acknowledged
again (one verify, one commit).

## The other half (M2a, stage 2)

**From the host.** `hand` = the other half's side makes a session for the
other half. It runs on the half with USB, where the chord is made (D4). The
other half must answer presence as the same release or a different version
(else MANIFEST and ARM get UNAVAILABLE): its side and pointing device from
presence are what ARM checks the manifest against, with this build's floors.
BEGIN, CHUNK and END then stage and verify the image in this half's slot
exactly as in M1. Then:

1. COMMIT (nonce, CRC low 16) in VERIFIED: state RELAYING. The image goes to
   the other half from this half's slot (below).
2. The other half holds the image and has verified it: state RELAYED.
3. COMMIT again: state SUBSIDE_COMMITTING. The other half commits and resets;
   this half keeps the link paused for its commit and reboot (2 s + 2.5 s,
   R18), probes it, and goes back to IDLE. This half never commits that
   image: the session never reaches COMMITTING, and commit step 0 would refuse
   its hand anyway.

ABORT works up to the second COMMIT, and tells the other half. A relay
failure latches ERROR with the reason: the other half's own refusal (BAD_SIG,
BAD_HASH, EPOCH, WRONG_HW, FLASH_ERR, ...), or UNAVAILABLE when it stopped
answering for 2 s, or TIMEOUT when it stayed in COMMITTING past the probes.
The 30 s session timeout still applies while relaying and
while RELAYED (D23: a host that went away stops the relay before the copy).
While a session for one hand runs, the session ops for the other hand get
BUSY. INFO with the other hand asks the other half over `KEYBOARD_UPDATE`
(the reply has INFO's layout without the JEDEC ID; UNAVAILABLE with state
`0xFF` if it does not answer; BUSY while a relay runs). RELAY (`0x0C`) shows
the relay's phase and progress.

**The relay** (`kb/updater/update_split.c`, from `updater_task()` on the half
with USB):

| Phase | Link | What happens |
|---|---|---|
| start, manifest | paused | ABORT (clears an old session there), then BEGIN: the signed manifest in 7 fragments |
| checking and erasing | paused | The other half checks the manifest against its own build and the signature itself (D7), then erases its slot a sector per pass; this half polls STATUS every 20 ms. A sector erase keeps the other half's interrupts off for about 50 ms, longer than the 20 ms serial timeout, so it erases a sector that needs it only after a request since its last erase and 5 ms after that request (`SVAL_UPDATE_SPLIT_ERASE_GAP_MS`): this half gets an answer between every two sectors, and an ABORT lands within a sector |
| sending | running | PAGE fragments read from this half's slot, at most 4 ms of RPCs per main-loop pass; both halves type between them |
| end, verifying | paused | END; the other half hashes its slot (SHA-512, structure, CRC); its CRC must equal this half's |
| verified | running | STATUS every 1 s keeps the other half's session alive until the host's COMMIT |
| commit, hold | paused | COMMIT; the other half commits 100 ms later from housekeeping with the M1 routine; the link stays paused 4.5 s. An answer of IDLE to COMMIT (it already rebooted, or lost the session), or no valid answer for 2 s (COMMIT may have been taken with its answer lost), also leads to the hold: the probe decides |
| probe | paused | STATUS. ERROR, or IDLE with an error: it did not commit (failure). COMMITTING: it has not begun its copy; probe again every 500 ms, and if it then falls silent (its copy) the whole hold again; at most 20 probes, then TIMEOUT. IDLE without an error: done. No answer (a new release may not talk to this one, V): done, flagged unconfirmed (RELAY flags bit1) |
| aborting | as it was | After an ABORT from the host, the session timeout, or a failure: ABORT once per pass until the other half answers (it may be erasing), for at most 2 s, and only then the link resumes. A relay whose link is already dead (no valid answer for 2 s) sends one ABORT and resumes at once |

A request without a valid answer is sent again unchanged (the other half
answers a retry from its cache); INVALID is accepted as a refusal only on the
fourth answer in a row, since a request damaged on the way is also answered
INVALID. No valid answer for 2 s fails the relay with UNAVAILABLE, then this
half sends one ABORT and resumes the link (in the commit phase it holds
instead, above).

After a relay ends done, this half shows the red error LED (and INFO flags
bit5) until presence first reads MATCH, also while presence reads NONE: a
release that changes the split table cannot talk to the old half at all, and
would otherwise look like an unplugged cable (V).

**The other half** (`update_split_slave_rpc` in the SlaveThread,
`update_split_slave_task` from housekeeping):

- The callback answers every request at once. It programs one 256 B page when
  a PAGE completes a page (R16) and does nothing else slow. The callback runs
  in the SlaveThread, which can preempt the main thread in the middle of a
  DMA transfer, and the PMW SPI's DMA reads its TX source from XIP (R6): so
  the program is refused with BUSY while any DMA channel is busy, checked
  with interrupts off in the same section as the program, and the fragment
  is given back (the relay sends it again).
- The request and response lengths come from the wire (`rpc_info`, behind a
  CRC-8 only) and are clamped to the 32-byte RPC buffers.
- The manifest and signature check, the erase, the image hash and the commit
  run in housekeeping, behind the pause. The two meet in a single-slot
  mailbox with a sequence number: the callback posts a job by changing the
  state, and moves the number with every new session and every cancel;
  housekeeping copies the job out under the split shared-memory lock (the
  callback holds the same lock), works on its copy, and posts the result only
  if the number and state are unchanged.
- No `KEYBOARD_UPDATE` request for 5 s drops the session (keeping any error as
  the reason, else TIMEOUT), and its LEDs go back to normal. Not once COMMIT
  is accepted.
- While its session runs it keeps full clock (no deep idle) and holds off the
  identity task, as `updater_active()` does on the half with USB.
- LEDs (D24): rainbow while it checks, erases, receives and verifies, magenta
  while it commits, red on an error.
- An image is applied only after COMMIT, and only with the CRC of the image it
  verified.

`sval_on_reconnect` (a settings write) is skipped while an update runs (D21).

**From `sval_update.py`.** `update FILE.svup` picks the half from the image's
hand. For the other half it waits for presence, prints the other half's INFO,
stages and verifies as in M1, relays (printing the phases), commits the other
half, then waits up to 10 s for it to answer again and prints its version and
presence; a fw that differs from the manifest's (both nonzero) is an error.
`pair A.svup B.svup` first checks both images against both halves' INFO (hand,
pointing device, test key or release build, epoch, storage format, size) and
sends nothing if either would be refused; then it does the other half, then
this half over USB (the chord twice, both on this half), waits for the board
to come back, and requires presence to show the same release on both halves
within 10 s and each half's fw to equal its manifest's (when both are set). `relay` prints the RELAY op;
`info --other` asks the other half; `info` also prints this half's version
string (INFO page 1). A build reports the `SVAL_FW_VERSION` it was compiled
with (`kb/updater/fw_version.txt` unless `make ... SVAL_FW_VERSION=N`), and
`make_update.py` copies it from the image's build-info record into the
manifest, so the two agree unless `--fw-version` says otherwise (which it
refuses when the record has a nonzero number). For an A/B pair of test images
build twice with `SVAL_FW_VERSION=2001` and `2002`.

## Release signing (M3)

**Keys (D30).** Two release key slots, `key_id 1` and `key_id 2`, whose public
halves are in `kb/updater/update_release_keys.h`. Every updater build compiles
both in; release builds have no test key. CI signs with one of them; the
other is the spare, so a lost or retired key can be replaced by a release
signed with the other. **M3 uses throwaway DRY RUN keys in both slots**
(`SVAL_UPDATE_RELEASE_KEYS_DRY_RUN 1`); their private halves are kept outside
the repository on one development machine. While they are in place, INFO
flags bit6 is set and release CI refuses every tag that is not a dry run.

**Swapping in the production keys** is a change to that one header plus the
secret:

1. On the signing machine, make two keys (each prints its public key):
   `make_update.py --genkey prod-1.key`, `make_update.py --genkey prod-2.key`.
2. Paste the two public keys into `update_release_keys.h` as `0x..` bytes, set
   `SVAL_UPDATE_RELEASE_KEYS_DRY_RUN` to `0`, and remove the DRY RUN banner.
   `make_update.py --print-release-keys` reads them back.
3. Put the seed line of the key CI signs with (normally key 1) in the
   `SVAL_UPDATE_SIGNING_KEY` secret of the `release-signing` environment, and
   keep both private keys offline.

Nothing else reads the keys from anywhere but the header. Boards on a release
with the DRY RUN keys accept only DRY-RUN-signed images, so the first release
with the production keys has to be installed by UF2 on them; dry-run tags are
prereleases, so no user board should run one.

**Versions (D17, proposed D32).** Launch tags are names (`vLaunch2`), so the
number is committed in `kb/updater/fw_version.txt` and the version string is
the tag name. Raise the number in the commit you tag. Release CI's first job
(`kb/tools/release_version.py`) fails unless the tag is a valid version string
(at most 16 of `A-Z a-z 0-9 . _ + -`) and the committed number is greater
than the one committed at the previous published release: the newest release
on the repository that is neither a draft nor a prerelease and is not this
tag (a release from before the file existed counts as 0). Prereleases are not
compared, so dry runs never use up a number. A tag whose name contains
`dryrun` or `dry-run` (any case), such as `vM3-dryrun1`, is a dry run: its
release is always a prerelease.

**Release CI** (`.github/workflows/release.yml`, on `v*` tags):

| Job | What it does |
|---|---|
| version | The check above. Outputs the version, the number and whether the release is a prerelease |
| build | The 12 release keymap builds as **release updater builds** (`SVAL_UPDATER=yes SVAL_UPDATE_RELEASE=yes SVAL_FW_VERSION_STRING=<tag>`), through `build-firmware.yml` with `updater: true`. Each runs `check_ram_funcs.py` and `make_update.py unsigned`, and uploads its `.uf2`, raw image, unsigned manifest and ELF. No build job can read the signing key |
| lint | `check_release_elf.py` on the 12 ELFs: an updater build whose build-info record says RELEASE and nothing else, two release key slots holding the header's keys, no test key anywhere in the image, no test-hook, keytest or host-bootloader symbols. `check_split_tables.py --golden`: every left/right pairing has the same split message table, and every build's table equals `kb/tools/split_table_golden.json` |
| publish | The `.uf2` files, to the tag's release (a prerelease for a dry run) |
| sign | In the `release-signing` environment, which needs approval for each run (D29): `make_update.py sign` for each image, then `make_update.py verify --build release`; publishes `<build>_<tag>.svup` next to each `.uf2` |

**This changes what release CI publishes:** every `.uf2` on a release is now an
updater build, not a default build: images of 122-127 KB instead of 94-99 KB, far below the 1,408 KiB cap. A default
`make` (no flags) is unchanged byte for byte. Halves running a default build
cannot talk to halves running an updater build (see "One release on both
halves"), so both halves must move to the same release, which they always do.

**What the signer refuses** (`make_update.py sign`): a key that is the test
key or neither slot of the header; a manifest that is not version 1, has
flags other than exactly RELEASE (so no DIAGNOSTIC), a keymap other than
`sval` or `blank`, an unknown hand or pointing device, another updater
protocol or an older storage format; an image whose length or SHA-512 does
not match the manifest or that fails the structure checks; an image without
a build-info record, or whose record is not a release build, has the test
key, test hooks, keytest or host bootloader, disagrees with the header on
DRY RUN, or reports another version than the manifest; an image that does not
contain each release key exactly once, or contains the test key; and a
manifest whose version or number is not the tag's (`--expect-version`,
`--expect-fw-version`). It then sets the manifest's `key_id` to the slot its
key fills, signs, and checks the signature.

**The golden split table** (`kb/tools/split_table_golden.json`) is the table of
the M3 release updater builds: 15 transactions (QMK's 12, plus the three keyboard
RPCs `KEYBOARD_SYNC_A`, `KEYBOARD_SYNC_B`, `KEYBOARD_UPDATE`), each with its
buffer sizes and shared-memory offsets. A change to the split table in a
release must regenerate it in the same commit, and the release notes must say
that both halves need the new release:

```
python3 -I keyboards/svalboard/tools/check_split_tables.py \
  --write-golden keyboards/svalboard/tools/split_table_golden.json BUILD.elf
```

**Signing locally**, the same two steps (the key file stays outside the
repository):

```
T=svalboard/trackball/pmw3389/right
F="SVAL_UPDATER=yes SVAL_UPDATE_RELEASE=yes"
make $T:sval $F SVAL_FW_VERSION_STRING=vM3-local
N=svalboard_trackball_pmw3389_right_sval
MU=keyboards/svalboard/tools/make_update.py
python3 -I $MU unsigned $N.uf2 --kb $T --keymap sval --release \
  --image-out $N.img --manifest-out $N.manifest
python3 -I $MU sign --image $N.img --manifest $N.manifest \
  -o $N.svup --key ~/m3-keys/dryrun-release-1.key
python3 -I $MU verify $N.svup --build release
```

`make_update.py verify FILE.svup --build release|test|plain` checks a `.svup`
offline as that kind of build would before erasing anything (signature with
that build's keys, manifest rules, hash, structure). `util/updater_test/run.sh`
with `SVAL_RELEASE_SVUPS="a.svup:ok b.svup:refused"` runs `.svup` files through
the C code of a release build (`tests/sval_updater/test_release.c`).

### Fork setup and the dry run (D31)

For whoever sets up `morganvenable/sval-qmk` (not done by any workflow). Run
the steps in order in one WSL shell: later steps use `R` and `MU` from
earlier ones.

1. The environment, with Morgan as required reviewer, tags only:

   ```
   R=morganvenable/sval-qmk
   ID=$(gh api users/morganvenable -q .id)
   E=repos/$R/environments/release-signing
   echo '{"reviewers":[{"type":"User","id":'$ID'}],' > /tmp/env.json
   echo '"prevent_self_review":false,' >> /tmp/env.json
   echo '"deployment_branch_policy":{"protected_branches":false,' >> /tmp/env.json
   echo '"custom_branch_policies":true}}' >> /tmp/env.json
   gh api -X PUT $E --input /tmp/env.json
   gh api -X POST $E/deployment-branch-policies -f name='v*' -f type=tag
   ```

   `prevent_self_review` is false because Morgan pushes the tag and approves
   the run himself.

2. The secret: the seed line of DRY RUN key 1, never echoed:

   ```
   cd ~/m3-keys
   grep -v '^#' dryrun-release-1.key | gh secret set \
     SVAL_UPDATE_SIGNING_KEY --env release-signing -R $R
   ```

3. The dry-run tag, on a commit of `feat/fw-updater` that is pushed:

   ```
   git -C ~/GitHub/sval-qmk tag vM3-dryrun1 origin/feat/fw-updater
   git -C ~/GitHub/sval-qmk push origin vM3-dryrun1
   ```

   Approve the `sign` job when it waits (Actions, the run, Review deployments).

4. Check the result:

   ```
   gh release view vM3-dryrun1 -R $R --json isPrerelease,assets
   gh release download vM3-dryrun1 -R $R -p '*.svup' -D /tmp/m3dry
   cd ~/GitHub/sval-qmk
   MU=keyboards/svalboard/tools/make_update.py
   for f in /tmp/m3dry/*.svup; do python3 -I $MU verify $f; done
   ```

   `isPrerelease` must be true, with 12 `.uf2` and 12 `.svup` files. The exit
   check on hardware (M3 exit): a test board running a release updater build
   of the dry run accepts its `.svup` with `sval_update.py update`, and refuses
   a test-key image with BAD_SIG.

A dry-run tag that fails part way can be deleted and pushed again under a new
name (`vM3-dryrun2`); delete its prerelease too.

## Tools and tests

- `kb/tools/make_update.py`: UF2 → `.svup` (manifest, signature, raw image). It
  refuses malformed UF2s and images the device would refuse. `unsigned`,
  `sign` and `verify` are the release steps (see "Release signing").
- `kb/tools/release_version.py`: release CI's version check (D32).
- `kb/tools/check_release_elf.py`: release CI's lint of the release ELFs.
- `kb/tools/sval_update.py`: the host tool (`list`, `info [--other]`, `status`,
  `relay`, `diag`, `abort`, `update`, `pair`, `reject`). Close Keybard first: raw
  HID replies reach every open handle.
- `kb/tools/check_ram_funcs.py BUILD.elf`: the RAM-code check above.
- `kb/tools/check_split_tables.py`: the split-table check across left/right
  pairings (P1), above, and against the golden table (`--golden`).
- `.github/workflows/updater-tests.yml` runs `util/updater_test/run.sh` on
  pushes and pull requests that touch the updater, its tools or its tests.
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
  `test_split.c` covers the split link layer: the pause against mocks of QMK's
  matrix and transport (a key held on the other half at pause entry is
  released, the exchange stops, this half keeps typing, resume re-arms the
  split watchdog), the `KEYBOARD_UPDATE` frames (every single-bit error is
  caught), PAGE fragment assembly, and presence and the mismatch rules.
  `test_relay.c` (in the `test_updater` and `test_commit` builds) runs both
  halves' code in one process over a simulated split link
  (`tests/sval_updater/test_session.c`): the relay start to finish (the other
  half erases and verifies only behind the pause, the pause is never held
  while the image streams, the per-pass budget holds), the other half's own
  refusals, faults on every RPC (dropped, damaged, duplicated, stale and
  reordered requests and answers), the link pulled in each phase (this half
  fails within 2 s and resumes the link, the other half drops its session 5 s
  after its last request, neither firmware changes), the 5 s timeout in each
  state, mailbox races (an ABORT or a new session between the copy-out and the
  result), idempotent PAGE, END and COMMIT, 20,000 random frames, a power cut
  on the other half after every flash operation of a relay (its old image
  boots, a new relay succeeds), the other half's commit with the real
  `update_commit.c` cut after every flash operation, `updater.c`'s session for
  the other half, and the version mismatch a half-done pair leaves (red LED,
  then MATCH once this half is updated too). `test_relay_review.c` adds the
  review fixes: the slave's timeout race (a request between its clock read and
  its lock), RPC lengths past the buffers, a busy DMA channel at a page
  program, the other half with its interrupts off while it erases and commits
  (every RPC then fails; a 125,184 B and a 256 KiB relay still verify, and an
  ABORT while it erases lands between sectors with the link still paused),
  the probe finding it still COMMITTING, every COMMIT answer lost, and the red
  LED until MATCH. `test_sval_update_tool.py` runs
  `sval_update.py update` with the other half's image and `pair` against the
  same simulation.
