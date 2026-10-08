# Settings storage

Svalboard keeps its settings (keymap, macros, Sval tables, pointing settings) in QMK's wear-leveling store: 128 KB of logical EEPROM, held in two identical 512 KB copies directly after the firmware.

| Flash | Contents |
|---|---|
| `0x000000`–`0x15FFFF` | Firmware (a linker check keeps it below `0x160000`) |
| `0x160000`–`0x1DFFFF` | Settings, copy 0 |
| `0x1E0000`–`0x25FFFF` | Settings, copy 1 |
| `0x260000`–`0xFFDFFF` | Free |
| `0xFFE000`–`0xFFFFFF` | Board identity: serial and name |

Firmware updates keep settings, as long as the settings layout is compatible. To carry settings across a reflash that changes it, export the layout file in Keybard first and load it again afterwards.

## Upgrading from the previous firmware

The previous firmware kept a dual-bank store at `0x200000`–`0x40FFFF`, and at `0x160000` an older copy of the settings in today's format. On the first boot after the upgrade, before settings load, the firmware erases `0x160000`–`0x40FFFF` (`settings_upgrade.c`), so that old copy is never read as current. This takes a few seconds, once. The board starts from defaults: load your layout file to restore your setup. A board that never ran the dual-bank firmware keeps the settings it already has at `0x160000`.

## What it protects against

Some boards running the Vial firmware reset to the default layout at random. The cause is in QMK's wear leveling: at boot, a checksum mismatch on the stored image was treated as a brand-new board, so one bad flash read discarded every setting, and the defaults written afterwards made the loss permanent.

Now:

- **A bad read** is retried, rereading the flash rather than its cache.
- **A damaged copy** is read from the other copy, then repaired.
- **Power lost during consolidation** leaves a complete copy: consolidation erases and rewrites one copy at a time, and the next boot repairs the other.
- **Power lost while a change is saved** can lose only that change, which was never confirmed. A log entry cut short at the end of a copy is ignored, and a copy whose log stops at an invalid entry keeps the entries before it; if the other copy reads completely, it is used instead.
- **Both copies unreadable**: the board starts from defaults and remembers that it did. Keybard tells you on its next connection to reload your layout file, and `SV_OUTPUT_STATUS` prints the same notice.

A single wrong write log entry that still reads as valid is not detected; QMK's log entries carry no checksum of their own. Your layout file remains the recovery for anything the board cannot read.

## Wear

Each copy wears at QMK's normal rate: a small change appends a few bytes to the write log, and each copy is erased only when its log fills. The second copy doubles the bytes written, not how often any sector is erased.

A consolidation happens when the log fills, about once per 98,000 key changes. It erases each copy one 64 KB block at a time, skipping blocks that are already erased and servicing USB between blocks, then rewrites both copies. The keyboard pauses for a few seconds while it does; on a test board the whole pause, including the host noticing it, measured about 4.5 s. A fresh board's first-boot format erases nothing, since its region is already blank.

## Tests

```
make test:wear_leveling_general test:wear_leveling_2byte test:wear_leveling_2byte_optimized_writes \
     test:wear_leveling_4byte test:wear_leveling_8byte test:wear_leveling_mirror test:wear_leveling_mirror_large
```

`wear_leveling_mirror` covers a damaged or erased copy, a copy missing the last entry, both copies damaged, a retried bad read, and power loss at every write and erase of a consolidating write. `wear_leveling_mirror_large` runs the same tests with a store large enough for multi-word log entries, plus an entry cut off at the end of a copy and copies whose logs stop at an invalid entry.
