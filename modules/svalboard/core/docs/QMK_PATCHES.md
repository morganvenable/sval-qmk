# Sval QMK core patches

The maintained [firmware-change catalog](../../../../keyboards/svalboard/docs/release/firmware-changes.md#complete-qmk-core-patch-inventory) lists the complete core patch surface and the benefit of each change. It currently includes 16 files in `quantum/` and `tmk_core/`, covering dynamic execution, one-shot timing, storage compatibility, keycode translation, wider macro APIs, wrapped HID replies, and USB identity.

The earlier five-file/~70-line inventory predates the storage and identity changes and must not be used as the current rebase scope.

## Merge/rebase checks

1. Compare all core changes with the actual upstream merge base; do not rely on a fixed line count.
2. Verify weak hooks for macro execution and raw HID sending still permit the module overrides.
3. Verify Sval dynamic introspection still owns combos, tap dances, and key overrides.
4. Check runtime one-shot timing against any upstream action changes.
5. Check NVM addresses, VIA's keycode-version byte, layout stamps, and reset ordering.
6. Review upstream keycode renumbering and update the translation table where supported. Preserve idempotence for interrupted upgrades.
7. Check the 256-macro count and 32-bit buffer APIs across declarations, host replies, playback, and storage.
8. Verify runtime product strings and prefixed serial descriptors retain their descriptor-size constraints.
9. Run `python3 -m unittest discover -s tests/sval_storage -v` and compile the maintained sensor/side builds.
10. Exercise migration, interrupted updates, identity persistence, and reconnects on hardware before claiming release-level coverage.

The [launch compendium](../../../../keyboards/svalboard/docs/release/README.md) is the user-facing entry point. [Client ID protocol](CLIENT_ID_PROTOCOL.md) documents communication routing separately from the core patch inventory.
