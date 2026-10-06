# Svalboard QMK vRC3

## Four more Keybard settings now take effect

vRC3 fixes four controls that vRC2 saved but did not apply to key behavior:

- **Tapping toggle count:** `TT(layer)` now uses the saved count. The default is **5 taps**. A saved value of **0 is treated as 1**: one tap toggles the layer, while pressing and holding still behaves as a momentary layer and releases normally.
- **Tap-code delay:** generated taps and text macros now use the saved press-to-release delay. The default is **0 ms**. Delays above 255 ms also work correctly; they no longer wrap around in the string/macro path. Encoder-map and DIP-switch-map delays remain compile-time settings.
- **Tap-hold Caps Lock delay:** generated Caps Lock taps and Caps Lock tap-hold bindings now use the saved delay. The default is **80 ms**.
- **Grave Escape overrides:** the saved Alt, Control, GUI and Shift options each force Escape when their modifier is held, in the same order Keybard displays them.

These settings take effect when saved and survive a keyboard reboot. **Review values saved in older firmware:** previously inactive settings will begin affecting behavior after this update. There is no storage-layout change or required settings reset.

## Validation

All 12 release firmware targets build, including both sides of the base, TrackPoint, PMW3360, PMW3389 and Azoteq variants, plus the two blank base-board images. A non-Sval RP2040 keyboard using Grave Escape and `TT()` also builds with its original compile-time settings.

All 1,106 QMK regression tests and 17 Svalboard host tests pass. The four controls passed 84 checks on a PMW3389-left FlipFET Mule through the on-board key-event harness, before and after reboot, with its original settings restored afterward. This validates the firmware action path; it does not replace physical switch, split-transport or pointing-device testing on every variant.

## Installing

Export a `.svil` backup in [Keybard](https://keybard.svalboard.com/) first. Choose the UF2 matching the sensor family and side of the half being updated:

| Hardware | Files |
| --- | --- |
| Base board | `svalboard_{left,right}_sval_vRC3.uf2` |
| PMW3389 trackball | `svalboard_trackball_pmw3389_{left,right}_sval_vRC3.uf2` |
| TrackPoint | `svalboard_trackpoint_{left,right}_sval_vRC3.uf2` |
| Azoteq | `svalboard_azoteq_{left,right}_sval_vRC3.uf2` |
| Empty base-board layout | `svalboard_{left,right}_blank_vRC3.uf2` |
| PMW3360 trackball (deprecated; only for units with this sensor) | `svalboard_trackball_pmw3360_{left,right}_sval_vRC3.uf2` |

Double-tap reset within 500 ms, then copy the matching UF2 to the **RPI-RP2** drive. Repeat for the other half as needed. Svalboard QMK uses Keybard, not the Vial or VIA configurator.

## Current limitations

- **Auto Shift:** not enabled in the standard firmware.
- **Alternate repeat:** modifier conditions and the default-alternate option retain the known matching problems described in the firmware review. Check custom mappings.

[Full guide](https://github.com/svalboard/qmk/blob/svalboard/keyboards/svalboard/docs/release/README.md) · [Firmware review](https://github.com/svalboard/qmk/blob/svalboard/keyboards/svalboard/docs/reviews/2026-10-04-qmk-fork-review.md)
