# Sval QMK core patches

The core patch surface currently includes 16 files in `quantum/` and `tmk_core/`, covering dynamic execution, one-shot timing, storage compatibility, keycode-version resets, wider macro APIs, wrapped HID replies, and USB identity.

The earlier five-file/~70-line inventory predates the storage and identity changes and must not be used as the current rebase scope.

## Settings storage (wear leveling)

`quantum/wear_leveling/` and the RP2040 backing store (`platforms/chibios/drivers/wear_leveling/wear_leveling_rp2040_flash.c`) are patched so that a bad flash read cannot silently reset the board's settings. See [settings storage](../../../../keyboards/svalboard/docs/settings-storage.md).

- `wear_leveling.c`: loading retries `WEAR_LEVELING_READ_ATTEMPTS` times. Only an erased image and checksum count as never consolidated; upstream treats any checksum mismatch that way. An unreadable store is erased and reported by `wear_leveling_data_lost()`, instead of replaying the log over zeros or stopping at a bad entry.
- `WEAR_LEVELING_COPIES`: mirrored copies of the backing store. Log entries go to every copy, consolidation rewrites one copy at a time, and loading takes the first copy holding data, repairing any copy that differs.
- `wear_leveling_internal.h`: `backing_store_erase_range()`, required when there is more than one copy.
- RP2040 backing store: ROM erase beyond `PICO_FLASH_SIZE_BYTES`, a JEDEC capacity check, uncached reads so retries reread the flash, and `backing_store_erase_range()`.
- Tests: upstream tests that seeded an invalid checksum beside a never-consolidated image, or expected a bad log entry to keep the entries before it, are updated; `wear_leveling_mirror` is new.

## Merge/rebase checks

1. Compare all core changes with the actual upstream merge base; do not rely on a fixed line count.
2. Verify weak hooks for macro execution and raw HID sending still permit the module overrides.
3. Verify Sval dynamic introspection still owns combos, tap dances, and key overrides.
4. Check runtime one-shot timing against any upstream action changes.
5. Check NVM addresses, VIA's keycode-version byte, layout stamps, and reset ordering.
6. Review upstream keycode renumbering and update the translation table where supported. Preserve idempotence for interrupted upgrades.
7. Check the 256-macro count and 32-bit buffer APIs across declarations, host replies, playback, and storage.
8. Verify runtime product strings and prefixed serial descriptors retain their descriptor-size constraints.
9. Run `python3 -m unittest discover -s tests/sval_storage -v`, `python3 -m unittest discover -s tests/sval_layers`, and every `make test:wear_leveling_*` suite (including `wear_leveling_mirror`), and compile the maintained sensor/side builds.
10. Keep the wear-leveling changes when upstream changes `quantum/wear_leveling/`: retries, the erased-only never-consolidated rule, `wear_leveling_data_lost()`, and copies.
11. Exercise migration, interrupted updates, identity persistence, and reconnects on hardware before claiming release-level coverage.

[Client ID protocol](CLIENT_ID_PROTOCOL.md) documents communication routing separately from the core patch inventory.
