# Durable settings storage

Svalboard uses two independent flash banks for the entire 128 KiB EEPROM image,
including keymaps, macros, programmable behaviors, pointing settings, and handedness.
The logical addresses and VIA layout stamp do not change. This replaces the old
single-copy wear-leveling driver, whose checksum failure could cause a factory reset.

## Flash allocation

All offsets are relative to the start of the flash die.

| Region | Offset | Size | Purpose |
| --- | --- | --- | --- |
| Previous Sval settings | `0x160000` | 512 KiB | Read-only upgrade source |
| Previous Vial settings | `0x1E0000` | 128 KiB | Untouched; not imported |
| Settings bank A | `0x200000` | 1 MiB | Snapshot and journal |
| Settings bank B | `0x300000` | 1 MiB | Snapshot and journal |
| Migration witness | `0x400000` | 64 KiB reserved | Prevent stale re-import |
| Board identity | `0xFFE000` | 8 KiB | Existing serial/name records, untouched |

The driver checks the JEDEC capacity before accessing the extended region. A
smaller or unrecognized die enters read-only mode. A linker assertion prevents
the firmware image from growing into the old settings region.

## Commit and recovery rules

Each bank contains a 256-byte header, a 128 KiB snapshot starting at offset
4096, and a page-aligned append journal. CRC-32 covers the snapshot, header
(including its generation), and each complete journal record (including its
address, length, and position). Journal payloads hold up to 236 bytes.

1. Ordinary writes append a checksummed page and verify its readback before
   changing the RAM cache. Unchanged bytes produce no flash writes.
2. When the journal fills, a replacement snapshot is written to the other bank.
   Its header is committed **last**, after verification. The active bank is never
   erased during this operation.
3. After settings and input have been quiet for five seconds, a task checked every
   15 seconds checkpoints changes. This also refreshes the fallback copy. Normal
   writes are already persistent; this is not a deferred-save timer. Background
   snapshots perform one erase or program per housekeeping iteration. Input pauses
   that work; a settings edit cancels the uncommitted snapshot and restarts later.
4. At boot, the newest valid generation is tried first. Checksummed reads are
   retried up to three times using uncached flash accesses. A failed snapshot or
   corrupt interior journal falls back to the other bank. A torn final journal
   page is ignored, preserving the verified prefix; the next change writes a new
   snapshot rather than programming over the damaged page.
5. If neither bank can be recovered, automatic initialization can only change RAM.
   It cannot erase either bank or import an obsolete copy from the old store.

Recovery produces a normal writable store; there is no permanent "suspect" flag
that keeps restoring an old mirror over subsequent edits.

## Upgrade and reset

The first boot imports the previous **Svalboard QMK** store without writing to it.
The old snapshot checksum, log bounds, lengths, and record types are checked.
The old log format had no per-record checksum; import cannot add retrospective
integrity guarantees. Invalid old storage remains untouched and requires recovery
or an explicit reset. A genuinely erased old store starts from defaults.

Two new snapshots are committed before the first migration completes. The
migration witness is recorded before accepting edits. A power cut during the
initial snapshot can therefore retry import; after migration, the old store can
never silently replace more recent settings. Downgrading to firmware with the old
driver sees the frozen pre-upgrade settings, not subsequent edits.

`SV_OUTPUT_STATUS` reports verified, imported, recovered, or read-only storage.
In read-only mode, configuration changes are temporary. Export anything useful
before resetting. An explicit `EE_CLR` keypress or the configured Bootmagic reset
gesture may leave read-only mode and create a fresh store. Ordinary boot-time
validation cannot authorize that erase. Reflashing the same firmware is not a
factory reset. A smaller flash die cannot be reset into supporting this driver.

## Guarantees and limits

- Interrupted snapshot creation preserves the previous committed bank. A torn
  journal page preserves the preceding verified records.
- A write larger than 236 bytes is multiple journal records, not one atomic
  transaction. Likewise, a complete host layout upload spans multiple calls;
  interruption may retain only the completed portion.
- Permanent damage to a whole bank may roll back changes since the last fallback
  snapshot. Both banks share one physical flash die; this is not protection against
  total chip failure. Keep exported backups.
- Quiet-time snapshots resume scanning between each 4 KiB sector erase and 256-byte
  program. Each individual operation still disables interrupts. Initial import
  writes two banks synchronously using 64 KiB erases. A full journal or explicit
  reset also needs a synchronous checkpoint. Measure boot, save, and rollover latency on hardware
  before release; the host test does not model flash timing or USB behavior.
- CRCs detect accidental damage; they are not authentication or error correction.

## Validation

Run `bash util/durable_storage_test/run.sh` for the real storage engine and legacy
decoder with a NOR-flash simulator, AddressSanitizer, and UndefinedBehaviorSanitizer.
Tests cover migration, transient reads, damaged snapshots, damaged journal tails,
all 529 snapshot erase/program interruption points, migration interruption,
repeated recovery/edit/reboot cycles, macro data, log rollover, and read-only and
explicit-reset behavior.

Before release, bench-test both halves: import an exported known layout, compare
keymap/macros/settings after flashing, interrupt writes and reboot repeatedly,
measure initialization and checkpoint pauses, and verify USB and split transport
recovery. Simulator tests do not substitute for this hardware validation.

## FlipFET left mule bench results (2026-10-06)

Tested `svalboard/left:sval SVAL_KEYTEST=yes` on a 16 MiB RP2040 mule.
The following checks passed:

- Verified full-flash backup before programming; firmware load readback verified.
- All 131,072 imported logical EEPROM bytes matched the decoded old store.
  The previous 512 KiB store and board identity bytes remained unchanged.
- A key binding and a two-character macro produced the expected captured HID
  reports before and after software reboot. Original settings were restored.
- Deliberately flipping a snapshot bit in the newest bank recovered the previous
  bank. New edits then persisted across further reboots; the complete original
  1,920-byte keymap was restored.
- Changing background erases from 64 KiB to 4 KiB reduced the maximum measured
  host HID round trip from 277.36 ms to 45.70 ms in separate 22-second samples.
  The latter sample had 631 requests and a 1.89 ms median. This is a host timing
  observation, not an upper bound on interrupt latency.

A further test damaged both snapshots, then stalled in Windows HID discovery
during recovery. This case is unresolved on hardware and is not counted as a
pass. The simulator's no-automatic-erasure test passes. Release validation still
needs this case resolved, physical power interruption during writes, initialization
and full-journal timing, and testing with a split partner. Software reboots and
synthetic reports do not demonstrate physical power-cut or sensor behavior.

The hardware-tested UF2 SHA-256 is
`f1d7000e332ecfaa69964907ae2b717615b627d8025bea33400819b62adde070`.
Private backups and machine-readable evidence are retained separately from git.
