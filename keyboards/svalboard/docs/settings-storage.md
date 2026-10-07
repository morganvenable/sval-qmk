# Settings storage

Svalboard keeps its settings (keymap, macros, Sval tables, pointing settings) in QMK's wear-leveling store: 128 KB of logical EEPROM, held in two identical 512 KB copies at flash offset `0x500000`. No earlier Svalboard firmware used that region, so the first boot after flashing this firmware starts from defaults; load your layout file to restore your setup. Later firmware updates keep settings, as long as the settings layout is compatible.

## What it protects against

Some boards running the Vial firmware reset to the default layout at random. The cause is in QMK's wear leveling: at boot, a checksum mismatch on the stored image was treated as a brand-new board, so one bad flash read discarded every setting, and the defaults written afterwards made the loss permanent.

Now:

- **A bad read** is retried, rereading the flash rather than its cache.
- **A damaged copy** is read from the other copy, then repaired.
- **Power lost during a write or consolidation** leaves at least one complete copy. Consolidation erases and rewrites one copy at a time; a copy left behind is repaired at the next boot.
- **Both copies unreadable**: the board starts from defaults and remembers that it did. Keybard tells you on its next connection to reload your layout file, and `SV_OUTPUT_STATUS` prints the same notice.

A single wrong write log entry that still reads as valid is not detected; QMK's log entries carry no checksum of their own. Your layout file remains the recovery for anything the board cannot read.

## Wear

Each copy wears at QMK's normal rate: a small change appends a few bytes to the write log, and each copy is erased only when its log fills. The second copy doubles the bytes written, not how often any sector is erased. A consolidation erases each 512 KB copy with interrupts off, which pauses the keyboard for about a second per copy; it happens only when the log fills, after many thousands of changes.

## Tests

```
make test:wear_leveling_general test:wear_leveling_2byte test:wear_leveling_2byte_optimized_writes \
     test:wear_leveling_4byte test:wear_leveling_8byte test:wear_leveling_mirror
```

`wear_leveling_mirror` covers a damaged or erased copy, a copy missing the last entry, both copies damaged, a retried bad read, and power loss at every write and erase of a consolidating write.
