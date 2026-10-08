# Plan: in-firmware updater (sval-qmk#7 / keybard#24), revision 4

Status: **plan for your review. No feature code has been written.** Sources: [morganvenable/sval-qmk#7](https://github.com/morganvenable/sval-qmk/issues/7), [svalboard/keybard#24](https://github.com/svalboard/keybard/issues/24), the WSL clone of `sval-qmk` (branch `feat/fw-updater`, cut from upstream `svalboard` at `7e0af09`) and a read-only reading of `keybard` (`main`). Paths are relative to each repo root, and firmware paths under `keyboards/svalboard/` are shortened to `kb/`. Every firmware citation was checked against `7e0af09`, and every pico-sdk and ChibiOS citation against the checked-out submodules (pico-sdk `d0c5cac`). **UNVERIFIED** marks anything not confirmed in code; two items remain, both datasheet-only: which blocks an AIRCR `SYSRESETREQ` resets on RP2040, and what state peripherals keep across a watchdog reset. Revision 2 took in two adversarial reviews, and revision 3 recorded your answers from the [decision page](https://claude.ai/artifact/WsV3uM75FY2iQ1p1XEsGsX). **Revision 4 applies the M0 findings:** upstream `7e0af09` removed `kb/storage/` and moved settings back to QMK wear leveling, so the storage-layer pieces are replaced; the commit routine gains the watchdog, boot2 and DMA safety fixes; a `REBIND` op answers the client-ID expiry; D12 is open again. The Appendix lists what changed.

---

## 1. Summary

- Add an updater that is compiled in only when a build flag is set. It stages a signed raw image in spare flash, verifies it, **invalidates the old image first** (page 0 programmed to zeros), then copies the new image from RAM and writes page 0 last.
- **M1** proves this on **one test board half over USB**, driven by `kb/tools/sval_update.py`. No split traffic is involved.
- **M2** adds the split relay (store-and-forward) and the version handshake, and needs the split test board. **M3** adds CI signing and release gating. **M4** adds the Keybard UI.
- **"Ready to test on hardware"** means: the right test board half running an `SVAL_UPDATER=yes` build, flashed once by UF2, on which the host tool can (1) rotate between images A and B, (2) get every bad-input case rejected with its own status code, (3) show the LED states, and (4) through a test-hooks build that halts mid-commit, come up as `RPI-RP2` and be recovered with `kb/tools/flash.sh`, with the settings and identity regions identical **byte for byte** to a dump taken before the test.

### Flash map (7e0af09, `kb/docs/settings-storage.md:5-11`)

| Flash offset | Contents |
|---|---|
| `0x000000`-`0x15FFFF` | Firmware (`kb/flash_reservation.ld:2` keeps it below `0x160000`) |
| `0x160000`-`0x1DFFFF` | Settings, wear-leveling copy 0 |
| `0x1E0000`-`0x25FFFF` | Settings, wear-leveling copy 1 (`kb/config.h:44-54`) |
| `0x260000`-`0xFFDFFF` | Free. The staging slot `0x800000`-`0x95FFFF` (D8) lies here |
| `0xFFE000`-`0xFFFFFF` | Board identity (`kb/identity.c:37-38`) |

### Corrections to the issue text

| Issue says | Code says | Plan |
|---|---|---|
| "Write sector 0 last" | Sector 0 also holds the vector table, which follows `.boot2` (`platforms/chibios/boards/common/ld/RP2040_FLASH_TIMECRIT.ld:44,98-102`). The ROM checks CRC-32/MPEG-2 (poly `0x04C11DB7`, init `0xFFFFFFFF`, no reflection, no xorout) over bytes 0-251 against the little-endian word at 252, tries 128 times, then falls back to USB boot (pico-bootrom `_flash_boot`; `lib/pico-sdk/src/rp2_common/boot_stage2/pad_checksum:39-43`). Test vector: page 0 of the current build gives `d58f0b07` | Zero page 0 first, write page 0 last |
| End with `watchdog_reboot()` | That is a flash function (pico-sdk `watchdog.c:77`), and it is not even linked into this firmware | Direct register writes from RAM (D10) |
| Fragment detection knows the side and the sensor | `sval_fragment_detect()` is a weak stub (`modules/svalboard/core/sval_fragments.c:40-44,74-83`). Handedness is `EE_HANDS` (`kb/config.h:37`), and the running build forces it on every boot (`quantum/split_common/split_util.c:155-172`; `-DINIT_EE_HANDS_LEFT` at `kb/left/rules.mk:1`) | The check is "image side and variant == running build's side and variant". It does **not** detect the hardware (Risk R8) |
| Erase may trip the split watchdog | The split watchdog only acts at boot (`split_util.c:82-120`), but a slave that reboots after an update needs a re-ping (D15) | The real limits are the 20 ms handshake timeout (`platforms/chibios/drivers/serial_usart.h:43-44`) and 10 errors to disconnect (`split_util.c:46-48`) |
| Image about 95 KB | The linker caps it at `0x10160000` (`kb/flash_reservation.ld:2`). The largest release image today is 97,268 B (trackball/pmw3360/right) | Slot and checks sized to 0x160000 |
| Raising RPC buffers is harmless | An old slave trusts `m2s_length` from the wire (`quantum/split_common/transactions.c:1050-1057`). Both halves XOR `NUM_TOTAL_TRANSACTIONS` into every handshake (`serial_protocol.c:59-61,69,136`) | Not an issue: both halves always run the same release (V), so buffer sizes always match. CI checks that every supported pairing of builds produces an identical split message table (P1) |

---

## 2. Milestones

### M0: setup, hardware facts, measurements (no firmware feature)

- [x] Clone [morganvenable/sval-qmk](https://github.com/morganvenable/sval-qmk) **with submodules** into Ubuntu WSL (`~/GitHub/sval-qmk`), add `upstream` = `svalboard/qmk`, and create branch `feat/fw-updater` from `upstream/svalboard` (`7e0af09`).
- [x] Re-check every pico-sdk and ChibiOS claim marked UNVERIFIED against the checked-out submodules. Done: 39 claims checked, 26 confirmed, 11 refuted (applied in rev 4), 2 still unverified (datasheet-only, listed in the header).
- [x] Build all 12 release builds (5 variants × 2 sides with `sval`, plus left and right with `blank`). All pass with Arm GNU 13.2.1; the largest image is 97,268 B, far below `0x160000`.
- [ ] On the chosen test board half (D1): enter `RPI-RP2` **by hand with the BOOTSEL button**, flash with `flash.sh`, and reboot. Record whether a RUN/reset button can be reached.
- [ ] Record the JEDEC ID. The 16 MiB check is capacity byte `rx[3] == 0x18` (`kb/identity.c:41,87-92`). It is UNVERIFIED that picotool 2.3.1 prints the raw JEDEC bytes; if it shows only the size, 16 MiB corresponds to `0x18`.
- [ ] Attach the board to WSL with usbipd (RP2 Boot, `2e8a:0003`). picotool 2.3.1 is available in WSL (it needs `LD_LIBRARY_PATH` set to its runtime). Dump, as bus addresses: the whole chip (`save -a`), settings `0x10160000`-`0x10260000`, identity `0x10FFE000`-`0x11000000`, slot `0x10800000`-`0x10960000`, as the baseline with SHA-256 sums.
- [x] Audit every DMA channel user for any source address in XIP. Result in R6: WS2812 reads RAM; the PMW SPI TX source `dummytx` is in XIP; I2C uses no DMA; core 1 never starts.

Exit: the fork builds; image size, JEDEC ID, BOOTSEL and reset access, picotool and the DMA audit are all recorded; the Section 3 decisions are answered.

### M1: one half updates itself over USB (no split test board)

**Scope:** staging slot, async state machine, manifest with structural and signature checks (test key), session binding, matrix-level gesture tied to a manifest hash, RAM commit with zero-first invalidation, a RAM vector table, a fed watchdog, LEDs, and STATUS. `hand` must equal the local side, otherwise `UNSUPPORTED`. There is **no split traffic**.

**Files** (new code in `kb/updater/` unless a path is given)

| Path | Purpose |
|---|---|
| `updater.c/.h` | State machine: `IDLE → MANIFEST_LOADING → CONFIRM_WAIT → VERIFYING_MANIFEST → ERASING → RECEIVING → VERIFYING_IMAGE → VERIFIED → COMMITTING`, plus `ERROR`. HID handler replies at once; all slow work runs in `updater_task()` from housekeeping, one block or verify slice per pass |
| `update_flash.c` | Erase and program for the slot only, with its own slot range guard. Follows the ROM-lookup pattern in `kb/identity.c:71-84` / `platforms/chibios/drivers/wear_leveling/wear_leveling_rp2040_flash.c:182-192` |
| `update_commit.c` | RAM-only commit (`__no_inline_not_in_flash_func`), RAM vector table, watchdog feed, reset. Compiled with `-fno-jump-tables -fno-tree-loop-distribute-patterns -fstack-usage`; no `switch`; no calls except to ROM pointers and listed RAM symbols. Owns a dedicated 4 KiB static sector buffer and its own 256 B boot2 copy |
| `update_image.c` | Structural checks on staged page 0 and the vectors (see the checks below) |
| `update_gesture.c` | Chord detection (D4) from the debounced local matrix (`matrix_get_row`, `quantum/matrix.h:65`), never from keycodes |
| `update_led.c` | LED states (D24): blue blink, white double flash on chord, rainbow, magenta, red 1 Hz 50/50. How rgblight is held off is decided by the D20 experiment |
| `update_manifest.h` | Manifest struct, status codes, op codes |
| `vendor/monocypher.c/.h`, `vendor/optional/monocypher-ed25519.c/.h` | Ed25519 verify (D7) with `crypto_ed25519_check` and `crypto_sha512`. Never `crypto_eddsa_check`: the core one uses BLAKE2b and does not verify standard Ed25519. About 14.1 KB of flash. BSD-2/CC0, not GPL |
| `kb/rules.mk` | `SVAL_UPDATER ?= no`, following `SVAL_KEYTEST` (`kb/rules.mk:46-50`). `SVAL_UPDATE_TEST_HOOKS ?= no` is a separate flag. `#error` if `SVAL_KEYTEST` and `SVAL_FIRMWARE_UPDATE` are both set (R11). `SVAL_UPDATER` builds set `USE_PROCESS_STACKSIZE = 0xC00` (main's stack is 2 KiB by default, `platforms/chibios/platform.mk:8-9`; the Ed25519 check needs 1,856 B; the extra 1 KiB fits in the free part of SRAM4). First D20 build check: `RGBLIGHT_DRIVER = custom` with a gating `rgblight_driver`; that build must then set `WS2812_DRIVER_REQUIRED = yes` itself, because only the `ws2812` driver sets it (`builddefs/common_features.mk:331-332`) |
| `kb/config.h` | `SVAL_UPDATE_BASE/SIZE/MAX_IMAGE`, gesture and LED constants |
| `updater.c` asserts | `MAX_IMAGE % 0x10000 == 0 && MAX_IMAGE <= WEAR_LEVELING_RP2040_FLASH_BASE`. Slot base and size 64 KiB aligned, base ≥ `WEAR_LEVELING_RP2040_FLASH_BASE + WEAR_LEVELING_BACKING_SIZE * WEAR_LEVELING_COPIES` (= `0x260000`), end ≤ `IDENTITY_SECTOR_A` (next to `kb/config.h:44-54`) |
| `kb/svalboard.c` | Call `updater_task()` from `housekeeping_task_kb` (`:503`). Add the channel case in `via_custom_value_command_kb` (`:614-631`). Skip `sval_rgb_idle_task` and `identity_task` while active |
| Settings hold-off | QMK's eeprom / wear-leveling path (including its multi-second consolidation, `kb/docs/settings-storage.md:37`) needs no hold-off for durability: writes go straight through (D12). The updater refuses `ARM`, and the commit refuses in step 0, while `wear_leveling_write_failed()` is latched (qmk#12) |
| `kb/matrix.c` | No low clock or deep idle while active: use the same gate as `scanlab_active()` in the pacing code (`kb/matrix.c:266-307`, gate at `:268`) |
| `modules/svalboard/core/client_wrapper.c/.h` | Add `client_wrapper_current_id()` getter (`wrapper_client_id` is static at `:19`, set at `:167`). This is a shared-module edit (D3) |
| `kb/tools/sval_update.py` | hidapi host tool modelled on `kb/tools/keytest.py` |
| `kb/tools/make_update.py` | UF2 → raw image. Refuses a family-ID mismatch, a start address other than `0x10000000`, gaps, or an end beyond `0x10160000`. Builds and signs the manifest |
| `kb/tools/check_ram_funcs.py` | Disassembles every symbol in the commit's `.time_critical` set. Fails on any branch or call whose final target (veneers and literal `ldr pc` followed) is neither in the allow-list nor in ROM (<0x4000), and on any `blx rN` other than the ROM pointers. Also fails on `-fstack-usage` > 512 B |
| `tests/sval_updater/`, `util/updater_test/run.sh` | Host tests under ASan and UBSan in a new standalone harness, modelled on the `util/durable_storage_test/run.sh` that `7e0af09` removed (see `7d71434`) (flash mock: see the host tests below) |
| `docs/updater.md` | Protocol, flash map, reset mechanism, recovery |

**Host protocol** (keyboard-local VIA channel, D2a). A packet is `[0xDD][client_id:4][0xFE]` + VIA (`modules/svalboard/core/client_wrapper.c:157-176`; overhead `client_wrapper.h:39`). Unwrapped packets also reach VIA (`modules/svalboard/core/sval.c:990-1003`), so **every op except INFO returns `INVALID` unless `client_wrapper_in_via()` is true** (`client_wrapper.c:50-52`). That leaves 23 bytes of `value_data` (same framing as `kb/scanlab.c:381-410`).

| Op | Request | Response / rule |
|---|---|---|
| `0x00 INFO` | `[hand]` | status, state, protocol version, slot base and size, max image, JEDEC ID (3 B), variant ID, hand, numeric version, storage format |
| `0x01 MANIFEST` | `[hand][off u8][n][≤20 B]` | Fills a 172 B RAM buffer (manifest 108 B + sig 64 B). Allowed only in IDLE or MANIFEST_LOADING |
| `0x02 ARM` | `[hand]` | Needs a complete manifest. Returns `nonce u32` (from ROSC, as `identity.c` does; not secret) and the first 4 B of `sha512(manifest)`, which the host shows the user. Binds the session to `client_id` and nonce. Enters CONFIRM_WAIT and the LED blinks |
| `0x03 BEGIN` | `[hand][nonce]` | `ACCEPTED` straight away once the gesture is done. Housekeeping then runs VERIFYING_MANIFEST (sig, structure fields, hand, variant, epoch) and ERASING (one 64 KiB block per pass, only blocks with any non-0xFF byte, `len` rounded up) |
| `0x04 CHUNK` | `[hand][off u24 LE][n ≤ 18][data]` | status + next offset. Strictly in order. A duplicate earlier offset is acknowledged again without being rewritten. Refused until ERASING is done. Nonce is checked through the session's `client_id`, because there is no room for it in the packet (D3) |
| `0x05 END` | `[hand][nonce]` | `ACCEPTED`. Housekeeping runs VERIFYING_IMAGE: SHA-512 over the slot read through `XIP_NOCACHE_NOALLOC_BASE`, the structure checks, and a CRC32 of the image kept for the commit |
| `0x06 STATUS` | `[hand]` | state, bytes staged, erase progress, last error, verify ms, erase ms max |
| `0x07 COMMIT` | `[hand][nonce][crc32 lo16]` | Needs VERIFIED. The `crc32 lo16` must match the verified image, so a stray COMMIT cannot fire against the wrong image. This is not a security check. The device acks, and the commit starts 100 ms later from housekeeping (`kb/scanlab.c:65-87` pattern) |
| `0x08 ABORT` | `[hand][nonce]` | Accepted from the bound session, or from anyone while `ERROR` is latched. Resets state and LEDs. **No erase**: BEGIN erases anyway |
| `0x09 REBIND` | `[hand][nonce]` | Re-binds a live session to the sender's client ID when the nonce matches; the old ID is no longer accepted. Keybard sends it straight after each 50 s client-ID renewal, before its next updater op (D3). Wrong nonce: `OTHER_CLIENT`. No session: `INVALID` |

Every op must answer within 50 ms (host-tested). Keybard fails a command at 1000 ms (`keybard src/services/usb.service.ts:739-742`). The session times out after 30 s with no op. Status codes extend the existing sets (`kb/identity.h:20-25`; the enum at `kb/keytest.c:18`): `OK, ACCEPTED, INVALID, UNAVAILABLE, BUSY, NOT_CONFIRMED, WRONG_HW, BAD_SIG, BAD_HASH, BAD_IMAGE, EPOCH, OUT_OF_ORDER, OVERRUN, TOO_LARGE, TIMEOUT, OTHER_CLIENT, UNSUPPORTED, FLASH_ERR`.

```c
typedef struct __attribute__((packed)) {
    uint32_t magic;           // 'SVUP'
    uint16_t manifest_ver;    // 1
    uint8_t  key_id;          // 0 = test, 1/2 = release keys
    uint8_t  hand;            // 0 left, 1 right
    uint8_t  pointing_id;     // this half's pointing device only (D18): 0 none, 1 TrackPoint,
                              // 2 trackball PMW3360, 3 trackball PMW3389, 4 Azoteq; append-only
    uint8_t  keymap_id;       // 1 sval, 2 blank: display only, never checked (D18)
    uint16_t flags;           // bit0 RELEASE, bit1 DIAGNOSTIC (scanlab/keytest/host-bootloader)
    uint16_t security_epoch;  // refuse < device epoch (D19)
    uint8_t  storage_format;  // refuse < current v2 format (D19)
    uint8_t  updater_proto;   // updater protocol of the image
    uint32_t image_len;       // 0x200..MAX_IMAGE, multiple of 256
    uint32_t fw_version;      // numeric, also used in split presence (D17)
    char     version[16];     // NUL-padded display string
    uint8_t  sha512[64];      // over image_len bytes
} sval_update_manifest_t;     // 108 B, followed by a 64 B Ed25519 signature over these bytes
```

**Structure checks on the device** (at VERIFYING_MANIFEST where possible, at VERIFYING_IMAGE, and again at commit step 0): the boot2 CRC of staged page 0 is valid (CRC-32/MPEG-2 over bytes 0-251, stored little-endian at 252; the host tool computes the same and a host test pins it with the `d58f0b07` vector); the initial SP at 0x100 lies in `[0x20000000, 0x20042000]`; the reset vector is odd and inside `[0x10000100, 0x10000000+image_len)`; `image_len` is a multiple of 256 and ≤ MAX_IMAGE; a release build refuses `key_id 0` or `flags.DIAGNOSTIC`.

**Commit routine** (RAM only, PRIMASK set, ROM functions through `rom_func_lookup_inline` as in `kb/identity.c:71-84`, no SDK flash wrappers, no `memcpy`/`memset`; buffer = a dedicated 4 KiB static sector buffer, not shared with QMK; process stack 2 KiB (3 KiB in `SVAL_UPDATER` builds) and exception stack 1 KiB, both in SRAM4, `RP2040_FLASH_TIMECRIT.ld:34,68-72`. The commit runs only from housekeeping, and on a slave only behind the split pause)

0. With IRQs on: refuse with no erase if `wear_leveling_write_failed()` is latched (D12); hold off the identity task; re-run the structure checks and SHA-512 on the slot; take the commit's own 256 B RAM copy of boot2 from page 0 and check its CRC (every boot2 copy is filled from `BOOT2_ROM`, which *is* page 0, `platforms/chibios/vendors/RP/stage2_bootloaders.c:10,151`; after step 2 it would copy zeros, so the copy is taken here and never refilled; do not rely on the `identity.c:68,171` copy, which exists only if `identity_init` ran); show the "writing" colour, latch it with `ws2812_flush()` (twice), then let the last WS2812 transfer finish (≥ `WS2812_TRST_US`; its DMA reads RAM, `ws2812_vendor.c:138,297`, so it is not stopped, and aborting it would corrupt the colour).
1. Set PRIMASK. Read every DMA channel's BUSY bit; if any channel is busy, clear PRIMASK and refuse the commit with `BUSY` and **no erase** (the PMW SPI TX source `dummytx` is in XIP; it is idle whenever housekeeping runs, R6). Fill all 48 slots of a RAM vector table, aligned to 256 B, with SP and the RAM fault handler (in `.time_critical`; it clears scratch4 and triggers the watchdog), and point VTOR at it (VTOR normally points at the flash table: `platforms/chibios/vendors/RP/RP2040.mk:13`; `RP2040_FLASH_TIMECRIT.ld:44-45`). Arm the watchdog (D9) in this order: write `PSM_WDSEL = 0x1FFFC` (at `0x40010008`; `PSM_WDSEL_BITS & ~(ROSC|XOSC)` as `_watchdog_enable` does at `watchdog.c:39-40`, which this firmware never links; WDSEL resets to 0, `psm.h:299-304`, and with 0 a watchdog reset resets nothing); clear scratch4; `WATCHDOG_LOAD = period_µs × 2` (erratum E1: the counter drops 2 per µs, `watchdog.c:55-59`; ≤ `0xFFFFFF` ≈ 8.39 s; ≥ 2 × the worst block erase); clear the `PAUSE_DBG0/1` and `PAUSE_JTAG` bits (`WATCHDOG_CTRL` resets to `0x07000000`, `regs/watchdog.h:22`); set ENABLE.
2. **Invalidate:** program page 0 to all `0x00`, then read it back through the NOCACHE alias. If it is not all zero, reset (the old image is still mostly intact; R1). Test halt point `N=-1` is here.
3. Erase sector 0 (4 KiB), then the rest of block 0, then the other 64 KiB blocks up to `round_up(image_len, 64 KiB)`, clamped to MAX_IMAGE. The RAM range guard rejects any address outside `[0, MAX_IMAGE)`. Feed the watchdog after every ROM call by writing `WATCHDOG_LOAD = period_µs × 2`.
4. For each 4 KiB sector: copy slot → RAM sector buffer with XIP on, exit XIP, program its pages (sector 0 starts at page 1), flush the cache, re-enter XIP through the boot2 copy taken in step 0 (never refilled). Feed the watchdog.
5. Compute CRC32 over `[0x100, image_len)` through NOCACHE and compare with the CRC32 from VERIFYING_IMAGE. On a mismatch, **erase the bad sector, program it again, compare again**, once. If it still fails, leave page 0 zero, then reset (→ BOOTSEL).
6. Program page 0 from the RAM buffer and read it back. Feed the watchdog.
7. Reset (D10): zero every RAM buffer that held image bytes (at least every word equal to `0xCAFEB0BA`), because the double-tap magic is noinit RAM (`.ram0.bootloader_magic`, `platforms/chibios/bootloaders/rp2040.c:32-33`) and could land inside the old buffer in the new image's layout, sending it to BOOTSEL. Confirm `PSM_WDSEL` is still set, clear scratch4, then write the watchdog trigger. Never call flash.

**M1 hardware tests** (the test board half from D1, USB only; picotool dumps before and after every row marked D)

| # | Step | Expected |
|---|---|---|
| 0 | M0 checks done: BOOTSEL by hand works, JEDEC ID recorded, baseline dump | Recorded |
| 1 | `flash.sh` the `SVAL_UPDATER=yes` build | Normal boot. `info` shows slot, JEDEC ID, variant, version |
| 2 | `make_update.py` builds A and B (differing only in version), signed with the test key | `.svup` files produced. `make_update.py` refuses a doctored UF2 with a gap or the wrong base |
| 3 | `update B` with no gesture | `NOT_CONFIRMED` after the 30 s window. Firmware unchanged |
| 4 (D) | `update B` with the gesture | LED states in order. `info` shows B. Dumps identical |
| 5 | Remap the gesture keys over VIA, then `update A` | The gesture still works (it is matrix-level). Remapped keycodes are not emitted during CONFIRM_WAIT |
| 6 | Reject matrix: wrong hand, wrong variant, `len > max`, `len` not a multiple of 256, a flipped signature byte, a bad SHA, bad boot2 CRC, bad SP or reset vector (signed with the test key), epoch below, storage format below, `key_id 0` on a release-flagged build, out-of-order chunk, overrun, END too early, an unwrapped packet, a second client ID, ABORT from another client, REBIND with the wrong nonce, COMMIT with the wrong crc, a 30 s stall | Each gives its own status. Error LED, then normal after ABORT |
| 7 | Unplug USB during ERASING, RECEIVING and VERIFIED | Old image boots. The slot is ignored |
| 8 (D) | `SVAL_UPDATE_TEST_HOOKS` build, halt at N = −1 (after zero, before erase), 0 (after first erase), mid-program, last sector, and after page 0. Unplug and replug each time | N ≤ last sector: `RPI-RP2` appears. `picotool save -r 0x10000000 0x10000100` shows page 0 zeros; `flash.sh` recovers. After page 0: B boots. Dumps identical |
| 9 (D) | Same halts with the watchdog armed (D9a) and no unplug | Before page 0: resets into BOOTSEL by itself within the watchdog period. After page 0: boots B |
| 10 | Test-hooks build with an injected HardFault after invalidation | Resets into BOOTSEL quickly through the RAM vector table |
| 11 | Two quick taps of the **RUN/reset button** within 500 ms (`platforms/chibios/bootloaders/rp2040.c:39-55`; magic is in RAM, so unplugging does not work) | BOOTSEL. Skip if there is no reset button (M0) |
| 12 | Slow transfer (`--delay`) idle for > 10 s | No dimming, no deep idle, LEDs held |
| 13 | Measure: flash and RAM cost, HID round trip, transfer time, verify ms, max erase ms per block, commit ms, stack margin of the Ed25519 verify (stack canary) | Logged. These decide D11; D13 is measured in M2b |
| 14 | Commit an image pair both ways (A→B and B→A) | Each boots its new image, never BOOTSEL (stale double-tap magic, step 7) |
| 15 | Run the D9a halt test with SWD detached, or with `PAUSE_DBG` cleared | Resets into BOOTSEL within the watchdog period (the watchdog does not freeze) |
| 16 | `update B` with the host tool renewing its client ID every 50 s and sending REBIND after each | Update completes; the old ID is refused after REBIND |

**Host tests (no hardware):** state ordering; every op answers within 50 ms; manifest bounds; REBIND (right nonce, wrong nonce, old ID refused); Ed25519 vectors; structure checks; boot2 CRC vectors (CRC-32/MPEG-2, `d58f0b07`, all-`0x00` page `7065399a` and all-`0xFF` page `0b8fd31a` both failing). The flash mock models a torn erase or program as random values in the affected sector or page, injects a cut after **every** flash op, and asserts on every op:
(a) during staging only `[SLOT_BASE, SLOT_BASE+SIZE)` changes;
(b) during the commit only `[0, round_up(image_len, 64 KiB))` changes;
(c) nothing else ever changes;
(d) after any cut, the boot2 CRC of page 0 is invalid **or** the whole new image is present.

**Exit:** every hardware row passes on the test board half; the host tests pass; `check_ram_funcs.py` is clean; you have reviewed the measurements in #13 and the answers to D11 and D13.

### M2: split relay and version handshake (M2a without the test board, M2b on the test board)

**Scope:** store-and-forward relay (D14) over a new `KEYBOARD_UPDATE` split message, presence with version (mismatch detection only), a split pause done in the keyboard layer (D15), slave mailbox, inactivity timeout, LEDs on both halves.

| Path | Purpose |
|---|---|
| `kb/svalboard.h:57-68`, `kb/svalboard.c:437-452` | The presence request layout stays the same. `kb_sync_listener` now fills the ≤32 B response (`transport.h:32`): magic, struct version, `fw_version u32`, short git hash, updater protocol, hand, pointing_id, crc8. The master passes `sizeof(presence_rpc_t)` (17 B) as the response length (`kb/svalboard.c:519-520`), so the caller changes too. On a version mismatch: Keybard warns, and the half with USB shows the red error LED until fixed (V). Today nothing is written to `out_data`, so the stale bytes fail magic and crc |
| `kb/matrix.c` (D15) | Override the weak `matrix_scan()` (`quantum/matrix_common.c:164`): while paused, skip `matrix_post_scan()` (`matrix_common.c:91-117`) and call `matrix_scan_kb()` directly. On pause entry, clear the slave rows once and report a change, as the disconnect path does (`matrix_common.c:100-108`), so a slave key held at pause entry is released. On resume, call `split_watchdog_update(false)` (`quantum/split_common/split_util.h:14`). The pause also covers the presence RPC (`kb/svalboard.c:513-529`) and the Scan Lab relay (`kb/scanlab.c:403`); M0 confirmed nothing else sends split messages. Refuse to pause while the link is disconnected. The pause flag defaults to false, because `bootmagic_scan` calls `matrix_scan` before init (`quantum/bootmagic/bootmagic.c:63-67`). `kb/matrix.c:124-125` declares the core `raw_matrix` and `matrix` with `ROWS_PER_HAND` rows: fix them to `MATRIX_ROWS`, or put the override in its own file. **No QMK core change.** The copied lines get a comment pointing at the core original, checked on each QMK merge |
| `updater/update_split.c` | **Master:** after the M1 path has staged and fully verified the subside image in its own slot, relays it from flash with a per-pass time budget. **Slave:** the `KEYBOARD_UPDATE` callback programs one 256 B page per RPC itself (at most ~3 ms). It runs in the HIGHPRIO SlaveThread (1 KiB stack, `platforms/chibios/drivers/serial_protocol.c:17-38`), not in housekeeping; the 20 ms budget is the master's wait for `GET_RPC_RESP_DATA`, and the master sends nothing else between `EXECUTE_RPC` and `GET_RPC_RESP_DATA` (`transactions.c:1035-1046`). The callback is idempotent by offset and seq. Erase, SHA, signature and commit run in slave housekeeping behind the pause, through a single-slot mailbox with a seq number; housekeeping takes the shmem lock when it copies out. The slave runs at full clock during an update (deep idle lowers the clock and takes long naps, `kb/matrix.c:276-292`, `kb/power.c:100`) |
| `updater.c` | `hand` = other side means: stage locally, then relay. `0xFF` if the subside does not answer (`kb/scanlab.c:399-407`) |
| `kb/svalboard.c:533` | Gate `sval_on_reconnect` (D21) |
| Slave | Abort after 5 s with no `KEYBOARD_UPDATE` traffic and restore the LEDs. An image is applied only after COMMIT |

Split ops: `INFO, BEGIN (async erase, paused), PAGE(off,seq,crc16,data), STATUS, END (async verify), COMMIT, ABORT`. Each page goes as several RPCs carrying ≤ 23 B of data (32 − op 1 − off 3 − n 1 − crc 4; `quantum/split_common/transport.h:28`), assembled in the slave's page buffer and programmed on the last fragment. Both halves are the same release, so there is no size negotiation. The subside verifies the signature itself unless D7 says otherwise.

**Update order (V):** the half without USB is relayed, verified and committed first. Straight after its COMMIT, the half with USB runs the M1 path on itself over USB. It does not wait to hear from the other half, which may not be able to talk to the old firmware. After both have rebooted (about 2.5 s: 500 ms double-tap window at `rp2040.c:50`; 2 s USB detect at `split_util.c:66-75`), presence must show the same version on both halves within 10 s. If the other half doesn't answer, Keybard tells the user to plug USB into it and copy the `.uf2`.

**M2a (no test board):** host tests for relay, mailbox, idempotent PAGE, slave timeout; CI check (P1) that every supported left/right build pairing produces an identical split message table, including each entry's buffer sizes and shmem offsets.
**M2b (test board):** update the subside, then the main half; pull TRRS mid-relay (the slave aborts within 5 s, both halves stay on the old image, LEDs restore); cut subside power mid-commit (BOOTSEL, recovered over its own USB); a forced version mismatch (Keybard warning plus red LED on the half with USB); measure the RPC round trip and the subside transfer time (target < 30 s); type on the subside during the relay and check no keys are lost outside paused windows, and that a key held at pause entry is released.

**Exit:** P1 passes; every M2b case passes; the measurements are reviewed.

### M3: signing, CI, release gating

| Path | Purpose |
|---|---|
| `.github/workflows/build-firmware.yml` | Also produces the raw image and the manifest. A signing job runs in an environment that needs approval, and **signs only release keymaps** with `flags.DIAGNOSTIC` clear (D22) |
| `.github/workflows/release.yml` | Publishes `*.svup` next to each `.uf2` |
| `.github/workflows/updater-tests.yml` | Host tests, a new workflow |
| Lint | Release updater builds: no test key, two release key slots, no test hooks, no `SVAL_KEYTEST`, no `SVAL_HOST_BOOTLOADER`. Golden transaction table matches the last release, including the per-ID buffer size and shmem offset fields of `split_transaction_table` dumped from each ELF |
| Version | `SVAL_FW_VERSION` (numeric + string) from the tag (D17) |

**Exit:** a dry-run tag on the fork produces a signed `.svup` that the M1 tool accepts with the release key and refuses with the test key. The "last manual update" release notes are drafted, following the release wording rules.

### M4: Keybard UI (fork `morganvenable/keybard`, PR to `svalboard/keybard`)

- New `src/services/updater.service.ts`, a sibling of `scanlab.service.ts`.
- A send path with **no** silent re-bootstrap retry (`usb.service.ts:700-722`). Client-ID renewal keeps its normal 50 s period (`usb.service.ts:17,490,586-596`); after each renewal the updater service sends `REBIND [hand][nonce]` before its next op (D3).
- UF2 unpacking, browser-side signature check, the manifest hash shown before the gesture, progress from STATUS, subside first, ≥ 10 s reconnect wait.
- Test on [keybard-test](https://morganvenable.github.io/keybard-test/) before production.

---

## 3. Decision record (answered 2026-10-08, revised in rev 4)

Your answers from the [decision page](https://claude.ai/artifact/WsV3uM75FY2iQ1p1XEsGsX), applied throughout this revision. Your notes are quoted.

### Policy

| # | Decision |
|---|---|
| **V** | **Both halves always run the same release.** Mixed versions are not a supported configuration. The half without USB is updated first. Straight after its commit, the half with USB updates itself over USB without waiting to hear from the other half; both are checked once both have rebooted. On a version mismatch, Keybard warns and the half with USB shows the red error LED until it's fixed. Consequences: no transaction-table freeze (old D16 dropped), no buffer-size negotiation, no RPC bounds-check patch. |
| **P1** (new, follows from D18) | Halves of one board can run **different builds** of the same release (e.g. left trackball, right TrackPoint). CI checks that every supported left/right pairing of builds produces an identical split message table (`NUM_TOTAL_TRANSACTIONS`, the order of IDs, and each entry's buffer sizes and shmem offsets). Each side uses its own table's sizes on the wire (`serial_protocol.c:75-76,87-88,142-150`), so a struct size change with the same IDs would desync the halves. A pointing-device option that adds a split message on one side would fail this check. All 12 release builds match today. |

### Answered

| # | Decision | Answer |
|---|---|---|
| D1 | M1 test unit | **Right test board half**, which has a reset button, so M1 #11 is in. M0 still checks its flash chip is 16 MiB |
| D2 | Host channel | **Keyboard-local VIA channel**, like Scan Lab. The updater enforces the client wrapper itself, through a small getter in `client_wrapper.c` |
| D3 | Session identity | Client ID plus the ARM nonce. **Revised in rev 4:** Keybard keeps its normal 50 s client-ID renewal and, after each renewal, sends `REBIND [hand][nonce]`, which re-binds the session to the new ID. Pausing renewal would not work: the firmware rejects IDs older than 120 s, measured on `timer & 0xFFFF0000`, so an ID really lives 65.5-131 s (`client_wrapper.c:24-47,136-138`), and a session (30 s gesture wait, erase, about 5,300 CHUNKs, verify) can outlive it |
| D4 | Confirmation | **Index South + Middle South, held 1 s**, on whichever half has USB: right `[6,0]`+`[7,0]` (`M` and `,` on the default keymap), left mirrored `[1,0]`+`[2,0]` (`V` and `C`). Read from the matrix, never keycodes; both keys are swallowed while waiting. Your note: *"flash the LEDs or something to register this event"*. So when the chord is recognised, the LEDs flash bright white twice (about 300 ms) before moving to the next state |
| D5 | Chord binding | The chord approves one specific image (its manifest hash) |
| D6 | Signatures in M1 | Ed25519 with a test key from the start |
| D7 | Crypto | Monocypher on both halves (`crypto_ed25519_check`); revisited for M2 once M1 has timed it |
| D8 | Staging slot | `0x800000`, size `0x160000`. It lies inside the free region `0x260000`-`0xFFDFFF` (7e0af09 map); asserted against `WEAR_LEVELING_RP2040_FLASH_BASE + WEAR_LEVELING_BACKING_SIZE * WEAR_LEVELING_COPIES` and `IDENTITY_SECTOR_A` |
| D9 | Watchdog during commit | On, fed after every flash operation. Arming sets `PSM_WDSEL` first, loads `period_µs × 2` and clears `PAUSE_DBG` (step 1) |
| D10 | Reset | Watchdog-triggered reset from RAM, with `PSM_WDSEL` set. Not `mcu_reset` (AIRCR `SYSRESETREQ`, `platforms/chibios/bootloaders/rp2040.c:16-18`), which runs from flash and whose reset scope on RP2040 is UNVERIFIED |
| D12 | Settings before commit | **Answered after rev 4:** no flush. Settings writes go straight through to flash (`sval_save()` is a no-op; eeconfig and dynamic-keymap write through), so there is nothing to flush. The one failure mode is a whole boot where the wear-leveling backing store failed to initialise and every write silently stayed in the RAM cache. [svalboard/qmk#12](https://github.com/svalboard/qmk/pull/12) latches that as `wear_leveling_write_failed()` (Sval `GET_INFO` byte 18 bit 1). The updater refuses `ARM` with `UNAVAILABLE`, and the commit refuses in step 0 with no erase, while that latch is set. **qmk#12 is a prerequisite for M1.** |
| D14 | Relay model | Store and forward |
| D15 | Pausing the split link | **Svalboard overrides the weak `matrix_scan()`** and skips the split exchange while paused. No QMK core change. Pause entry clears the slave rows once; resume calls `split_watchdog_update(false)` so a rebooted slave is re-pinged |
| D17 | Version in presence | 4-byte version number plus short git hash in presence; full string through INFO |
| D18 | Hardware ID | **One field per component, per half.** Each half's image names only its own pointing device (`pointing_id`) plus its side, and each half's image drives only its own device. Switching to a different pointing device is allowed after a Keybard confirmation naming both. The keymap (`sval`/`blank`) is recorded for display only and never checked |
| D19 | Downgrades | Any signed image at or above the security floor and the v2 settings format |
| D20 | LED takeover | A quick experiment in M1 decides between holding off rgblight output and overriding the WS2812 flush. First build check: `RGBLIGHT_DRIVER = custom` (`common_features.mk:312`; `rgblight_drivers.c:6-14`); fallback `rgblight_disable_noeeprom()` (`rgblight.h:326`) |
| D21 | Settings write on reconnect | Skipped during an update; the general flash-wear fix becomes a separate issue |
| D22 | Signing scope | Release keymaps of every variant only |
| D23 | Computer sleeps mid-update | Abort before the copy; ignore once the copy has started |
| D24 | LED colours | As proposed: blue blink (waiting for the chord), white double flash (chord recognised, from D4), rainbow (erasing and receiving), solid magenta (writing), red (error). Your note: *"1Hz 50/50 duty cycle"*, so the error blink is 500 ms on, 500 ms off. Fixed brightness about 96 |
| D25 | Build flags | `SVAL_UPDATER` and a separate `SVAL_UPDATE_TEST_HOOKS`, both off by default |
| D26 | Flash chip not 16 MiB | Updater reports "unavailable"; Keybard falls back to the guided manual copy |
| D27 | M1 host tool | Python, modelled on `keytest.py` |

### Still open, by design

| # | Decided when |
|---|---|
| D11 | USB chunk pacing: after the M1 #13 measurements |
| D13 | Split message size: after the M2b measurements. Size negotiation is no longer needed (V) |
| D16 | Dropped (V). Replaced by P1 |

---

## 4. Risks and mitigations

| # | Risk | Mitigation / test |
|---|---|---|
| R1 | Power cut during erase leaves a valid boot2 with garbage behind it (boot loop; only physical BOOTSEL helps) | Zero page 0 first and check it (step 2). A zeroed or blank page 0 fails the CRC, so the ROM falls through to USB boot; a partial zero-program leaves a valid CRC only by chance (about 2^-32). Mock tears; M1 #8 N=−1 and N=0 |
| R2 | Branch into flash while XIP is off (veneers, `blx rN`, libgcc case helpers) | Flags, no switch, `check_ram_funcs.py` with an allow-list and veneer following. Only the divider and int64 helpers are wrapped (`RP2040.mk:85-98`) |
| R3 | Fault while the vector table is in erased flash | RAM VTOR table, all 48 slots → watchdog reset. M1 #10 |
| R4 | Commit takes longer than the watchdog period, or the watchdog resets nothing | Feed after each ROM op; load ≥ 2 × the worst-case op, ≤ 8.39 s; `PSM_WDSEL` set before arming. Max op time measured in #13; M1 #9 and #15 |
| R5 | Stack overflow in RAM code or in the Ed25519 verify | Dedicated sector buffer; `-fstack-usage` ≤ 512 B checked; main's stack raised to 3 KiB in `SVAL_UPDATER` builds, verify called at a shallow depth, margin measured with a canary (M1 #13) |
| R6 | DMA or core 1 reads XIP during flash ops | Core 1 never starts (`platforms/chibios/boards/GENERIC_RP_RP2040/configs/mcuconf.h:36`). DMA users are WS2812 (RAM source, `ws2812_vendor.c:138,297`) and the PMW SPI, whose TX source `dummytx` is in XIP (`lib/chibios/os/hal/ports/RP/LLD/SPIv1/hal_spi_lld.c:55,312,396`) but idle whenever housekeeping runs (SPI calls are synchronous, `platforms/chibios/drivers/spi_master.c:463-467`). I2C uses no DMA. The PMW SROM upload is CPU byte writes (`drivers/sensors/pmw33xx_common.c:131-133`). Mitigation: commit only from housekeeping, BUSY check and refusal with no erase in step 1 |
| R7 | Signed but malformed image boots into garbage | Device-side structure checks (M1); `make_update.py` refusals; M1 #6 |
| R8 | Wrong-hand image passes (a half already on the wrong build) | Documented: the check is build-vs-image only. Recovery is a normal UF2. Option: record the side in identity at manufacture (deferred) |
| R9 | Updater erases settings or identity (offset vs XIP mixups; flash map in `kb/docs/settings-storage.md:5-11`) | Static asserts, RAM range guard, mock invariants (a)-(c), picotool dumps byte for byte |
| R10 | Unauthenticated ops or session hijack from a web page | Wrapper required, session bound, ABORT bound with no erase, gesture tied to the hash, signature, compiled off by default. The client ID is not a secret (D3) |
| R11 | Host forges the gesture (`kb/keytest.c:76,104` `action_exec`) | Matrix-level detection; `#error` on KEYTEST + UPDATER |
| R12 | Downgrade to a vulnerable, pre-v2 or diagnostic image | Epoch, storage floor, DIAGNOSTIC flag, M3 signing scope |
| R13 | Every HID op times out (> 1000 ms) | Async BEGIN and END; 50 ms host test |
| R14 | Two builds that are meant to pair (e.g. left trackball, right TrackPoint) disagree on the split message table | P1 CI check across supported pairings, sizes and offsets included |
| R15 | One half sends a larger RPC than the other expects | Cannot happen: both halves are the same release (V), and P1 checks pairings |
| R16 | Slave misses RX while IRQs are off for a page program | Program inside the RPC callback, in the SlaveThread (master idle), so there is no async IRQ-off window. The RX FIFO is 8 deep (joined, `serial_vendor.c:386`) at 1 Mbaud (`kb/config.h:170`): about 80 µs of headroom. M2b measures errors |
| R17 | Abandoned slave session; callback/housekeeping race | 5 s slave timeout; mailbox with seq; shmem lock on copy-out |
| R18 | Master stalls for about 2 s after subside COMMIT | Stay paused for commit time + 2.5 s. M2b measures |
| R19 | Keybard renews the client ID mid-session, or the ID expires | D3: normal renewal plus `REBIND` with the nonce. Host tests; M1 #16 |
| R20 | Updater LEDs overwritten by rgblight sync | D20 |
| R21 | Deep idle or clock change mid-update | Forced full clock on both halves. M1 #12 |
| R22 | Estimates wrong (HID and RPC round trips, erase time, verify ms) | Measured in M1 #13 and M2b. Plan revisited before M2 and M4 |
| R23 | Stale double-tap magic: after the watchdog reset the new image finds `0xCAFEB0BA` in noinit RAM (old image bytes) and enters BOOTSEL. Deterministic for a given image pair | Zero the image buffers before the trigger (step 7). M1 #14 |
| R24 | Peripherals keep state across the watchdog reset: external sensors stay powered (UNVERIFIED, datasheet only) | The new image initialises them as at power-up. M1 #4 and M2b check typing and pointing after an update with no power cycle |

---

## 5. Out of scope / deferred

- keybard#24 options A (guided copy) and B (WebUSB PICOBOOT).
- A/B slots, rollback, monotonic anti-rollback counters, encryption, delta updates.
- Runtime hardware detection (fragments stay a stub); recording the side in identity (R8).
- `sval_on_reconnect` wear fix (D21b); the stale `SVAL_HOST_BOOTLOADER` comment and `docs/scan-lab.md` reference (`kb/scanlab.h:39`) as separate issues.
- Svalboard's `bootmagic_lite()` override at `kb/svalboard.c:551` is dead code: core now calls `bootmagic()` and the weak `bootmagic_scan`, so bootmagic is live in release builds. A separate issue, unrelated to the updater.
- Keybard Paranoid support; updating boards that still run Vial firmware.

## 6. Where the work happens

- **Firmware:** the WSL clone of [morganvenable/sval-qmk](https://github.com/morganvenable/sval-qmk) (with submodules), branch `feat/fw-updater` from `upstream/svalboard`, then a PR to `svalboard/qmk`. The Windows reference clone (`GitHub/svalboard-qmk`) stays read-only.
- **Builds:** Ubuntu WSL only. Do not run concurrent `make` on the same target (they share `.build`). CI: only the Svalboard release and lint workflows, plus the new updater host-test workflow.
- **Hardware:** the test board only (D1), **never the daily-driver right half**. Recovery is BOOTSEL + `kb/tools/flash.sh`.
- **Keybard (M4):** a worktree of `GitHub/keybard-fork` from `upstream/main`, a PR to `svalboard/keybard`, keybard-test first.
- **Gates:** all decisions are answered (Section 3). [svalboard/qmk#12](https://github.com/svalboard/qmk/pull/12) must be merged (or the branch rebased onto it) before M1 code. M0 hardware results come to you before M1 code starts; M1 #13 numbers come to you before M2.

---

## Appendix: reviewer notes

**Revision 4 (M0 findings, against `7e0af09`):**

- Storage layer replaced: flash map, asserts, dump ranges, ROM-call and JEDEC pattern (`kb/identity.c`, `wear_leveling_rp2040_flash.c`), dedicated 4 KiB sector buffer, settings hold-off, new test harness and CI workflow.
- Commit safety: `PSM_WDSEL` set before arming, `LOAD = µs × 2`, `PAUSE_DBG` cleared; boot2 copy taken in step 0 and never refilled; commit only from housekeeping, refused with no erase if any DMA channel is busy; WS2812 left to finish; full 48-slot RAM vector table; image buffers zeroed before the trigger.
- Design gaps: `REBIND` op (D3); main's stack 3 KiB for Ed25519; slave rows cleared on pause entry and split watchdog re-pinged on resume (D15); P1 compares sizes and offsets.
- D12 reopened, then answered: refuse while `wear_leveling_write_failed()` is latched (qmk#12); no flush. Citations updated; UNVERIFIED markers resolved except AIRCR reset scope and peripheral state after a watchdog reset.

All blockers and major points from both reviews were checked against the source and accepted. Spot-checked: `client_wrapper.c` has 182 lines; `sval.c:990-1003` lets unwrapped packets fall through to VIA; `identity.c:37-38,41` hold the identity sectors and the `0x18` check; RPC buffers are 32 B (`transport.h:28,32`); `usb.service.ts:17` renews every 50 s; `kb/config.h:155` enables double-tap unconditionally; `keytest.c:76,104` calls `action_exec`.

| Point | Note |
|---|---|
| "Reserve IDs in M1 so M1 and M2 builds interoperate" | Accepted, with a caveat: a board running an M1 build cannot talk to a half on today's release. Superseded in revision 3: halves always run the same release (V), so no IDs are reserved ahead of time |
| "Program pages inside the RPC callback" | Accepted for M2, but the claim that the master sends nothing else during the callback rests on `transaction_rpc_exec` being sequential (`transactions.c:1035-1046`). Measured in M2b, not assumed |
| "PIO RX FIFO overflows during each program" | The RX FIFO is 8 deep (joined, `serial_vendor.c:386`) at 1 Mbaud (`kb/config.h:170`), about 80 µs of headroom. The real IRQ-off windows are short (`serial_vendor.c:188-192,219-231,274-285`). Mitigated anyway (R16) |
| "Don't call rgblight_disable (persists)" | Correct, but `rgblight_disable_noeeprom()` exists (`rgblight.h:326`), so it is listed as D20(c) |
| Sector buffer location | Rev 3 reused the store cache. That cache is gone in `7e0af09`; rev 4 uses a dedicated 4 KiB static buffer rather than QMK's `wear_leveling.cache`, so the updater does not depend on QMK internals |
| "Retry once" | The reviewers were right: NOR flash needs an erase before reprogramming. Step 5 now erases, programs and compares again |
| Wrong citations | Fixed in rev 2: client_wrapper `:157-176/:105/:19/:167`; `RP2040.mk:85-98` is the divider/int64 wrap, not memcpy; full mcuconf path. Rev 4 re-checked every citation against `7e0af09` and the checked-out submodules |
