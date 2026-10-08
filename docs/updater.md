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
| `SVAL_UPDATER=yes` | The updater: VIA channel `0x55`, staging slot, commit routine, LED states. Accepts images signed with the TEST-ONLY key (`key_id 0`) |
| `SVAL_UPDATE_TEST_HOOKS=yes` | Adds the `TEST_HALT` op and the commit halt points (hardware tests only) |
| `SVAL_UPDATE_RELEASE=yes` | Release updater build: no test key; DIAGNOSTIC images and `key_id 0` refused. Cannot be combined with test hooks |

`SVAL_UPDATER` cannot be combined with `SVAL_KEYTEST` (keytest injects key
events, so a host could fake the confirmation chord). Only release builds may
ever ship: any other updater build accepts images signed with the test key,
whose seed is in the repository (`kb/tools/sval_update_TEST_ONLY.key`).

```
make svalboard/trackball/pmw3389/right:sval SVAL_UPDATER=yes
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
| `0x00 INFO` | - | state, protocol, slot base/size and max image (4 KiB units), JEDEC ID, pointing ID, hand, fw version, storage format, security epoch, flags (16 MiB die, settings writes failing, release build, test hooks), last error |
| `0x01 MANIFEST` | off, n ≤ 20, bytes | bytes filled, of 172 (108 B manifest then 64 B Ed25519 signature). In order; offset 0 restarts. Ties the load to this client |
| `0x02 ARM` | - | nonce u32 and the first 4 bytes of SHA-512(manifest), which the host shows. Checks the manifest fields, binds the session to client ID + nonce, starts the 30 s chord window |
| `0x03 BEGIN` | nonce | ACCEPTED once the chord is made: signature check, then the erase, one 4 KiB sector per main-loop pass |
| `0x04 CHUNK` | off u24, n ≤ 18, bytes | next offset. Strictly in order; an earlier chunk is acknowledged and not written again |
| `0x05 END` | nonce | ACCEPTED: SHA-512, structure checks and CRC of the staged image, 2 KiB per pass |
| `0x06 STATUS` | - | state, bytes staged, erase progress, last error, timings, CRC low 16 bits once VERIFIED, flags |
| `0x07 COMMIT` | nonce, CRC low 16 | ACCEPTED; the commit runs from housekeeping 100 ms later. The CRC (CRC-32/MPEG-2 over image bytes `[0x100, len)`) must match the verified image |
| `0x08 ABORT` | nonce | From the session, or from anyone while ERROR is latched. Never erases |
| `0x09 REBIND` | nonce | Moves the session to the sender's client ID (Keybard renews its ID every 50 s); the old ID is refused after it |
| `0x0A TEST_HALT` | nonce, point, fed | Test-hooks builds only, in VERIFIED: where the commit stops (see Test hooks). INVALID elsewhere |

States: `IDLE 0, MANIFEST_LOADING 1, CONFIRM_WAIT 2, VERIFYING_MANIFEST 3,
ERASING 4, RECEIVING 5, VERIFYING_IMAGE 6, VERIFIED 7, COMMITTING 8, ERROR 9`.
Status codes: `OK 0, INVALID 1, UNAVAILABLE 2, FLASH_ERR 3, ACCEPTED 4, BUSY 5,
NOT_CONFIRMED 6, WRONG_HW 7, BAD_SIG 8, BAD_HASH 9, BAD_IMAGE 10, EPOCH 11,
OUT_OF_ORDER 12, OVERRUN 13, TOO_LARGE 14, TIMEOUT 15, OTHER_CLIENT 16,
UNSUPPORTED 17` (`kb/updater/update_manifest.h`). A refused request (another
client, a wrong nonce, a request out of order) changes nothing. A failed update
(a check fails, a flash error, OVERRUN, a timeout, a refused commit) latches
ERROR until ABORT. A session with no op for 30 s times out.

**Confirmation.** After ARM, hold Index South + Middle South for 1 s on the half
being updated. On the right half that is matrix `[6,0]+[7,0]` (`M` and `,` on
the default keymap); on the left it is `[1,0]+[2,0]` (`V` and `C`). The chord is
read from the scanned matrix, never from keycodes, and both keys are hidden from
QMK while the updater waits.

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

| Point | Where | Expected (M1 #8 / #9) |
|---|---|---|
| `invalidated` (1) | N = -1: page 0 zeroed and read back, nothing erased | BOOTSEL |
| `first-erase` (2) | N = 0: sector 0 erased | BOOTSEL |
| `mid-program` (3) | after programming the middle sector | BOOTSEL |
| `last-sector` (4) | every sector programmed, page 0 still erased | BOOTSEL |
| `page0` (5) | page 0 written and read back, before the reset | the new image boots |
| `fault` (6) | a HardFault (`udf`) straight after the invalidation (M1 #10) | resets at once, into BOOTSEL |

By default the halted board spins with interrupts off and feeds the watchdog, so
it hangs until it is unplugged (M1 #8). With `--halt-unfed` it stops feeding, and
the watchdog resets it within 8 s (M1 #9, #15).

## Tools and tests

- `kb/tools/make_update.py`: UF2 → `.svup` (manifest, signature, raw image). It
  refuses malformed UF2s and images the device would refuse.
- `kb/tools/sval_update.py`: the M1 host tool (`list`, `info`, `status`, `abort`,
  `update`). Close Keybard first: raw HID replies reach every open handle.
- `kb/tools/check_ram_funcs.py BUILD.elf`: the RAM-code check above.
- `util/updater_test/run.sh [UF2 ...]`: host tests under ASan and UBSan. They
  cover `make_update.py`, the manifest, image and crypto checks, the session state
  machine, `sval_update.py` against the C state machine, and the commit routine.
  The commit tests run `update_commit.c`'s real step logic against a mock die and
  mock registers (`tests/sval_updater/update_commit_host.h`). They cut the power
  after every flash operation of staging and of the commit, both cleanly and
  torn (random bytes in the sector, block or pages the operation touched), and
  then check that staging changed only the slot, the commit changed only
  `[0, round_up(len, 64 KiB))`, nothing else changed, and page 0 is either invalid
  or the whole new image is in place.
