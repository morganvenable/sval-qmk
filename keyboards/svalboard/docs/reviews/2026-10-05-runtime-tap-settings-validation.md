# Runtime tap settings validation — 2026-10-05

Validated `fix/runtime-tap-settings`, starting at `4fa8b6677d16ba9a0d0b8a3275615d75f367586e` (base `b642ca7f6d74442aaeab7f4e42b86e0072fd1f1d`), with fixes and tests committed as `94fa94de8e234c1bb9a0a03aab048541b48ff372`. All final commands below exited **0**. No existing test assertions were changed.

## Fix found during validation

The new runtime getter returns a 16-bit tap-code delay, but the string helpers accepted an 8-bit interval. A regression test reproduced 256 ms becoming 0 and 300 ms becoming 44. `send_string_interval_t` now preserves 16 bits under `SVAL_ENABLE`; other keyboards retain the original `uint8_t` API. The RAM, character and program-memory string helper signatures agree. This does not change storage, protocol version or setting IDs.

The comment describing a zero tapping-toggle count was also corrected: zero aliases one, so a single tap toggles the layer, while a held press releases normally. It does not turn `TT()` into `MO()`.

## Builds and tests

Ubuntu/WSL2, Arm GNU Toolchain 13.2.Rel1, native GCC/G++ 13.3, clang-format 19.1.7. Release target lists were checked against both release workflows. Each command in the following table exited **0**; no compiler warnings appeared in the 12 release builds.

| Command | Result |
| --- | --- |
| `make -j4 svalboard/left:sval` | Built |
| `make -j4 svalboard/right:sval` | Built |
| `make -j4 svalboard/trackpoint/left:sval` | Built |
| `make -j4 svalboard/trackpoint/right:sval` | Built |
| `make -j4 svalboard/trackball/pmw3360/left:sval` | Built |
| `make -j4 svalboard/trackball/pmw3360/right:sval` | Built |
| `make -j4 svalboard/trackball/pmw3389/left:sval` | Built |
| `make -j4 svalboard/trackball/pmw3389/right:sval` | Built |
| `make -j4 svalboard/azoteq/left:sval` | Built |
| `make -j4 svalboard/azoteq/right:sval` | Built |
| `make -j4 svalboard/left:blank` | Built |
| `make -j4 svalboard/right:blank` | Built |
| `python3 tests/sval_runtime_tap/build_non_sval.py` | Built a temporary `handwired/onekey/rp2040` keymap with `TT(1)`, Grave Escape, all four compile-time overrides and nondefault timing constants |
| `make -j4 test:all` | **1,106 tests passed across 94 executables** |
| `python3 -m unittest discover -s tests/sval_keytest -v` | 10 passed |
| `python3 -m unittest discover -s tests/sval_layers -v` | 1 passed |
| `python3 -m unittest discover -s tests/sval_storage -v` | 3 passed |
| `python3 -m unittest discover -s tests/sval_runtime_tap -v` | 3 passed |
| `make -j4 svalboard/trackball/pmw3389/left:sval SVAL_KEYTEST=yes` | Built the instrumented Mule image |
| `qmk format-c --core-only <touched C/H/CPP files>` | Applied repository clang-format configuration |
| `qmk format-c --core-only --dry-run <same files>` | Clean |
| `git diff --check` | Clean |

The full QMK run includes existing tapping, combo, tap-dance, Auto Shift, Grave Escape and send-string coverage. Added QMK executables cover runtime settings (6 tests), Auto Shift (7), combos (7), and the non-Sval compile-time path (3). Auto Shift and combo variants repeat the six base cases under their respective feature flags and each add a dedicated test of the runtime delay path that was previously compiled out at a zero default.

The new Python tests compile the complete production settings implementation against simulated EEPROM, checking defaults, reload, zero-toggle normalization and full-width values. They also verify recovery after a transport exception and refusal to overwrite an existing recovery journal. New Python files were formatted with the repository YAPF configuration.

Before the interval fix, `make -j4 test:sval_runtime_tap` exited **2**, reproducing the truncation. After the fix it exited **0**. The final full-suite run above includes all new variants. QMK builds were serialized: independent `make` processes share a top-level error marker and should not run concurrently in one worktree.

## Hardware

Tested **FlipFET Left Mule**, serial `sval:E46498769F365934`, using the PMW3389-left image and the on-board event harness over Windows USB. The other connected board was not modified. The firmware captures internal report timestamps; host USB polling latency is not the measured delay.

**84/84 cases passed**, including immediate operation and operation after reboot:

- `TT()` counts 5, 3, 1 and stored 0; exact tap counts and held-key release.
- Tap-code and plain-text macro delays 0, 37, 256 and 300 ms.
- Caps Lock delays 0, 80, 130 and 300 ms through a generated macro tap, mod-tap and layer-tap.
- Each Grave Escape override independently disabled/enabled: Alt bit 0, Control bit 1, GUI bit 2, Shift bit 3. Alt and Control were combined with Shift so the override-off result differs from Escape. Tests also require released output at the end.

Original settings, bindings and macro bytes were restored and verified after reboot. A separate final full snapshot matched the pre-flash snapshot for every readable QMK setting, all 960 keycodes, all used feature-table entries, one-shot settings and the entire 106,664-byte macro buffer. The original 222 ms tapping term was preserved. The harness was inactive and idle after restoration. Backups containing user configuration remain local and are not committed.

The Mule remains on the tested instrumented firmware, with capture inactive. Its image SHA-256 is `2dd0eb909f0cc71844572b5e9473160fa77f56f458a9fccb58a9818630be510d`. The source included the interval fix before it was committed; the result artifact records that provenance explicitly.

[Captured synthetic events and results](../runtime-tap-settings-hardware-results.json).

This validates firmware behavior and persistence on the Mule. It does not claim physical switch testing, split-transport testing, or hardware validation of every pointing-device variant. Auto Shift has dedicated unit coverage but remains disabled in standard Svalboard firmware. Encoder-map and DIP-switch-map delays intentionally remain compile-time constants.

## Repeating the hardware checks

Build and flash `SVAL_KEYTEST=yes` firmware for the **test board's actual hardware**, then run with Python and hidapi on the host owning the USB device:

```sh
python keyboards/svalboard/tools/keytest_runtime_tap.py \
  --serial sval:E46498769F365934 \
  --backup /local/path/new-recovery.json \
  --output /local/path/results.json
```

The runner temporarily changes settings, four bindings and the first 16 macro bytes. It creates the serial-bound recovery journal before writing and restores in `finally`, rebooting and checking persistence. Choose a new journal path for each run. After a power loss or unavailable USB device, rerun the same command with `--restore` to restore from that journal. Keep the journal until restoration succeeds.

## Release notes

[Fresh vRC3 notes](../release/vRC3.md) describe the four now-working controls, defaults, zero-toggle behavior, full-width macro fix and validation limits. Previously saved inactive values begin affecting behavior on upgrade. The guide removes these four controls from current limitations and retains a version-qualified note for vRC2 and earlier. Published vRC2 notes are unchanged. Pushing this branch does not merge it into `svalboard`, create a tag or publish a firmware release.
