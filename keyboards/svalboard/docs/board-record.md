# Board record

Every main board (the RP2040 MCU board) can carry a factory record: which board it is, written once at the bench. The record lives in flash, because the RP2040 has no OTP, in a block that nothing but a whole-chip erase ever touches.

## Flash map (16 MiB die)

| Range | What | Written by |
| --- | --- | --- |
| `0x000000`–`0x15FFFF` | Firmware image | UF2 drag-drop, picotool, the updater's commit |
| `0x160000`–`0x25FFFF` | Settings: QMK wear leveling, two 512 KiB copies | QMK |
| `0x270000` (4 KiB) | Pair UF2 record | A pair UF2, then the boot relay |
| `0x800000`–`0x95FFFF` | Updater staging slot | The updater |
| **`0xF00000`–`0xF0FFFF`** | **Board record (this document)** | **The board ID station only, append-only** |
| `0xFFB000`–`0xFFCFFF` | Side records (either-hand builds) | Firmware |
| `0xFFD000` (4 KiB) | Build reset record | Firmware |
| `0xFFE000`–`0xFFFFFF` | Identity: USB serial and board name | Firmware |

Everything else is free.

The board record is one whole 64 KiB erase block of its own. No other region shares a 4 KiB sector or a 64 KiB block with it, so no erase aimed elsewhere can reach it, even a 64 KiB block erase. It sits far from the regions that might grow: the high records grow down from `0xFFB000`, and the staging slot ends at `0x960000`.

### Why it stays

- **UF2 drag-drop and picotool** write only the blocks in the file. Firmware is linked to end below `0x160000` (`flash_reservation.ld`), so it never reaches the record.
- **The updater** writes only its staging slot and the firmware area. `updater/update_flash.c` asserts at compile time that the slot, the commit's range and the pair record all end below the block. `slot_flash_op` also refuses the block at run time, next to the boot ROM calls. `make_update.py` refuses an image that reaches past `0x160000`, and it lists the block as protected for pair UF2s. A `.svup` carries only an image for the firmware area, at slot-relative offsets, so it cannot name the block.
- **The firmware** only reads the record (`board_record_sval.c`). It never erases or writes it.
- **The ID-writer** programs only erased pages inside the block. It has no erase code at all.
- Only a whole-chip erase (a "flash nuke") clears it.

Tests: `tests/sval_board_id/run.sh` and `tests/sval_updater/test_updater.c` (`test_board_record_untouched`).

## Format

The block holds 256 pages of 256 bytes. There is one record per page, written in page order into erased pages and never erased. The newest valid identity record is the board's current identity, and older ones stay as history. A page whose first word is erased is unused. An interrupted write leaves a damaged page that readers skip; the next write goes to the page after it.

Every record is little-endian:

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 4 | magic `SVBR` (`0x52425653`) |
| 4 | 1 | format version, 1 |
| 5 | 1 | kind: 1 = identity |
| 6 | 2 | length of the whole record, header to CRC |
| 8 | 8 | the flash die's 64-bit unique ID, as the writer read it |
| 16 | 4 | programming time, Unix seconds (UTC) |
| 20 | … | fields: `[tag][len][len bytes]` |
| length − 4 | 4 | CRC-32 (IEEE) over every byte before it |

| Tag | Field |
| --- | --- |
| 1 | model (text), e.g. `SvalMCUMini` |
| 2 | PCB revision (text), e.g. `v1.0b0` |
| 3 | batch or lot (text) |
| 4 | station name and tool version (text) |
| 5 | GP3 strap: class (0 low, 1 high, 2 floating, 3 unstable), then the number of high reads out of 64 under pull-up, pull-down and no pull |
| 6 | factory tests: u32 tests run, u32 tests passed (bit masks) |
| 7 | notes (text) |
| 8 | the strap class the batch expected |

Text is UTF-8 with no terminator. Readers skip tags and kinds they don't know, so new fields need no version change.

## Reading it: Sval `0x3A` BOARD_RECORD

Discovery: GET_INFO byte 20 (the third feature byte), bit 1 (`sval_flag3_board_record`, `0x02`).

Request: `[0xDF] [0x3A] [chunk]`

Response: `[0xDF] [0x3A] [status] [format=1] [length lo] [length hi] [chunk] [identity records] [flags] [16 bytes]`

The response carries 16 bytes of the current record, starting at `chunk × 16`. In `flags`, bit 0 means the firmware compared the record's unique ID with this flash die's, and bit 1 means they match. A record copied from another board's flash would not match. Status values:

- 0: a record
- 1: no record
- 2: the flash is not 16 MiB, so there is no region
- 3: the chunk is past the end

Older firmware answers `0xFF` in byte 1, for an unknown command. The host decodes the record with the table above.

The firmware looks at the block only when a host asks, and remembers just which page holds the record. A board without a record behaves exactly as it did before.

## The station

`tools/board_id/board_id.py` runs on Windows with picotool. Run `py board_id.py batch` once per batch, then `py board_id.py run`. The station handles one board in BOOTSEL at a time:

1. It reads the block and refuses a board that already has a record, unless given `--force`, which appends a newer record.
2. It loads the ID-writer into RAM. The writer is a RAM-only program; the station patches the batch's fields into it.
3. The writer checks that the flash is 16 MiB, reads the unique ID and the GP3 strap, and refuses on a strap mismatch. Otherwise it appends the record, reads it back, leaves a report in RAM and reboots into BOOTSEL.
4. The station reads the report and the record, optionally flashes firmware, and logs a CSV line.

Run `py board_id.py run --fake DIR` for a dry run against simulated boards. Set `BOARD_ID_VERBOSE=1` to print every picotool call.

Measured on a SvalMCUMini v1.0b0 (dry run, 2026-10-28):
- picotool 2.1.1 `load -x` starts a RAM image by rebooting into it, not with EXEC.
- The writer's report at `0x20010000` survives that reboot and the writer's own reboot into BOOTSEL.
- In BOOTSEL this board's USB serial is the generic `E0C9125B0D9B`. picotool's `--ser` matches the flash unique ID instead, so the station reads that ID with `picotool info -d` while exactly one board is in BOOTSEL, then pins every call and the deny list to it.
