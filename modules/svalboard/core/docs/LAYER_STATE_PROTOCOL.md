# Read-only active and default layer reporting

This additive extension keeps Sval protocol version **3**, the command registry, EEPROM format, and the existing `0x16` active-mask offset unchanged. Hosts discover support through `GET_INFO` feature flag **bit 6 (`0x40`, `sval_flag_default_layer_state`)**. Bits 4 and 5 are allocated by the separate tap-dance-context and context-layer feature branches; this extension does not reuse them or their commands.

## Wire contract

All requests use the existing mandatory client wrapper and a 32-byte HID report (hidapi writes prepend report ID zero). No new command or setter is introduced.

- `GET_INFO`: `DD <client:u32 LE> DF 00 ...`. The existing feature byte at full-report offset **19** includes `0x40` when the appended default mask is supported. The version at offset 7 remains 3; UID at offset 11 is unchanged.
- `LAYER_STATE_GET`: `DD <client:u32 LE> DF 16 ...`, no arguments.
- Response: `DD <client:u32 LE> DF 16 <active:u32 LE> <default:u32 LE> ...`.

| Full report offset | Inner Sval offset | Meaning |
|---|---|---|
| 7–10 | 2–5 | `layer_state`, unsigned little-endian 32-bit mask; unchanged for older hosts |
| 11–14 | 6–9 | `default_layer_state`, unsigned little-endian 32-bit mask; valid only with advertised capability |

Both globals are sampled in the same read handler; no state setter, EEPROM operation, or input injection is called. The handler requires at least ten inner bytes to write the complete response and otherwise returns the existing error command. The normal wrapper supplies 27 inner bytes. This does not introduce a firmware session ID or revision/sequence guarantee.

Masks may be zero, nonzero, or have multiple bits including bit 31. Hosts must preserve the entire mask, resolve the union of active/default bits in descending priority, and preserve QMK's layer-zero fallback. Never collapse defaults to the highest bit or substitute 1 for a reported zero.

## Compatibility

- New trainer + new firmware: read both masks from each poll; default-only changes refresh legends and highlights.
- New trainer + old firmware: the flag is absent, so bytes after the active mask are ignored even when nonzero. Manual default selection remains available.
- Old trainer/Keybard + new firmware: active mask remains at its original offset. Keybard `VialService.getLayerStateMask()` decodes one uint32 after the command echo and ignores the appended bytes. Existing feature checks mask their own bits; protocol version stays 3. No Keybard update is required for existing functionality.
- Reconnects negotiate `GET_INFO` again. A capability from a previous connection must not be retained after a downgrade.

This feature does not report physical presses, modifiers, held-key resolution, keymap revisions or macro execution. Existing clients can still use their existing layer-state setter; Sval Trainer never sends it.

## Validation

`python3 -m unittest discover -s tests/sval_layers -v` compiles the production info/layer read cases, feature enums and client wrapper with undefined-behavior sanitization. It checks capability composition, both masks and bit 31, legacy offsets, wrapper identity, response boundaries, short buffers and unchanged layer globals. Run storage/keytest regressions separately. Full sensor/side builds validate integration; no board is accessed or flashed by these tests.

### Ubuntu/WSL2 validation, 2026-10-04

- Packet/wrapper suite: 1 test compiling two capability configurations with UBSan; storage suite: 2 tests; keytest suite: 7 tests; all pass.
- `make <keyboard>/<side>:sval -j4` passed for `svalboard`, `svalboard/trackpoint`, `svalboard/trackball/pmw3360`, `svalboard/trackball/pmw3389`, and `svalboard/azoteq`, each left and right. `svalboard/left:blank` and `svalboard/right:blank` also passed (12 builds total).
- Builds used isolated dependency worktrees at the repository's recorded revisions and the installed Arm GNU 13.2.Rel1 toolchain. Generated UF2s remain local and untracked. No hardware or flashing was performed.
- Branch `feat/default-layer-reporting` is based on `feat/keytest-instrumentation` at `7e6a674d36`. Only this extension's commit is required when integrating onto another compatible Sval branch; reconcile capability declarations with context branches while preserving their bits 4–5.

## Setting the saved default layer

`DEFAULT_LAYER_SET` (`0x2B`) makes a layer the default now and after a restart, exactly as pressing `PDF(layer)` does (`set_single_persistent_default_layer`). Hosts use it when they renumber layers, so the default stays on the same layer.

- Request: `DD <client:u32 LE> DF 2B <layer:u8> ...`
- Response: `DD <client:u32 LE> DF 2B <status:u8>`: `0` = set, `1` = the keymap has no such layer (nothing changes).
- Discovery: GET_INFO byte 19 (inner Sval offset; full report offset 24) is a second feature byte, after the storage flags in byte 18. Bit 0 (`sval_flag2_default_layer_set`) advertises this command. Older firmware leaves the byte zero; older hosts never read it. The first feature byte is full (bit 4 is held by the tap-dance context branch, bit 7 is reserved).
- `0x2A` is `STORAGE_RESET_CLEAR`; don't confuse the two.
