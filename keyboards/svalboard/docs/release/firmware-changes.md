# What Svalboard QMK adds to your keyboard

[Explore Keybard](README.md) · [Release announcement](announcement.md) · [Protocol and companion apps](protocol.md)

Svalboard QMK builds on QMK’s keys, layers, shortcuts, and programmable behaviors. It adds the storage, editing, pointing, and hardware controls that let you configure your Svalboard through Keybard and keep using that setup after closing the editor.

## Change your setup without rebuilding firmware

| Addition | What it lets you do |
| --- | --- |
| Editable tap dances | Give one position separate tap, hold, double-tap, and tap-then-hold actions. |
| Editable combos | Turn a chord of up to four keys into another action. |
| Editable key overrides | Change a key’s output under selected modifier and layer conditions. |
| Alternate-repeat mappings | Associate a remembered key with another output for repeated editing patterns. Modifier matching has known limitations in this release. |
| Leader sequences | Assign an action to an ordered sequence of up to five keys, making shortcuts easier to remember. |
| 256 slots per feature | Keep a larger collection of tap dances, combos, macros, overrides, repeat mappings, and leaders. |
| 256 macros | Store more reusable text and action sequences than the previous 50-macro configuration allowed. |
| Extended macro actions | Combine text, key presses and releases, delays, layer actions, and Svalboard controls. |
| Supported typing and mouse-key settings | Change leader timing, modifier swaps, simultaneous-key reporting, and supported mouse-key behavior from Keybard. |

Tap dances, combos, and mod-taps work, but several timing controls currently have no effect even after you save them. See [Current limitations](README.md#current-limitations) for the affected settings.

## More room for your setup, easier updates

| Addition | Benefit |
| --- | --- |
| Expanded settings storage | Provides 128 KiB of shared space for the keymap, programmable behaviors, macros, and other settings. |
| Larger macro addressing | Lets macros use the available shared space beyond the previous 64 KiB boundary. The space left for macros depends on the other stored features. |
| Settings preservation across ordinary updates | Keeps a compatible layout when you install firmware built on another day. |
| Supported keycode translation | Carries older steno assignments forward when updating from the supported keycode versions. |
| Automatic migration from the supported Vial release | Imports an existing Svalboard layout, macros, programmable behaviors, pointing preferences, and other supported settings. |
| Migration progress tracking | Lets an unfinished copy retry from the old configuration and prevents a completed migration from reappearing after a later reset. |
| Preserved legacy configuration | Leaves the old Vial store intact during migration. Later changes in Keybard belong to the new setup and are not copied back into the old store. |
| Checked macro uploads | Rejects uploads that exceed the macro buffer or contain invalid offsets. |
| Wear-leveled storage | Spreads configuration writes across flash and avoids rewriting unchanged data. Ordinary typing does not continuously save the layout. |

Automatic migration supports **`svalboard/vial-qmk v2025-11-01` with the `vial` keymap**. Keep an exported backup and check your modifier preferences, timing settings, and macros after upgrading. Changes to the storage layout can still require a reset, and interrupted updates are not guaranteed to preserve every setting.

## A board that keeps its identity

| Addition | Benefit |
| --- | --- |
| Persistent serial number | Keeps each board identifiable across ordinary firmware updates and settings resets. |
| Your own board name | Makes multiple Svalboards easier to recognize in a device list. Save the name in Keybard, then restart the board to show it to the computer. |
| Redundant identity records | Keeps a previous identity record while saving an updated one. |
| Reliable long-name transfers | Transfers board names correctly through Keybard’s connection protocol. |

Your board name and serial live on the keyboard. Layer names and behavior names travel with your `.svil` backup; they do not currently synchronize automatically from the board to another browser.

## Pointing controls for each hand

| Addition | Benefit |
| --- | --- |
| Independent left/right pointer settings | Give each side its own sensitivity and cursor or scrolling role. |
| Per-pointer mouse-layer activation | Choose which pointer activates mouse-layer bindings. |
| Activation threshold, decay, and timeout | Reduce accidental activation and control the return to typing. |
| Sniper and Boost keys | Hold or toggle 2×, 3×, or 5× adjustments for precise placement or faster travel. |
| Scroll hold/toggle, axis lock, and natural scrolling | Switch into scrolling, keep motion on one axis, and choose your preferred direction. |
| Combined pointer and keyboard mouse controls | Use both pointing devices alongside keyboard mouse buttons and movement keys. |
| TrackPoint recalibration | Correct pointer drift on supported TrackPoint hardware. |
| Layer lighting | See a color cue for the layer you are using. |
| Split USB wake handling | Wake a suspended computer from the keyboard when the computer permits USB wake. |

Firmware variants support base boards, PMW3360 and PMW3389 trackballs, TrackPoint, and Azoteq pointing hardware. Choose the sensor and side that match the half you are updating.

## Lower power use during inactivity

| Addition | Benefit |
| --- | --- |
| Active, light-idle, and deep-idle scan rates | Scan frequently while typing and less often during quiet periods to reduce power use. |
| Trackball rest modes | Reduce sensor power during inactivity on supported hardware. |
| Idle lighting and processor sleep | Dim the lighting and reduce processor activity between scans. |
| Selectable deep-idle clocks and longer sleep intervals | Allow further idle power savings, with a possible delay before the first input after inactivity. |

Longer idle intervals and sensor rest modes can delay the first input after inactivity. Scan Lab displays estimated current consumption for comparing settings; actual consumption depends on the hardware and configuration.

## Advanced diagnostics and firmware testing

These tools support troubleshooting and firmware development. Normal keyboard setup does not require adjusting optical timing.

| Addition | Purpose |
| --- | --- |
| Scan Lab timing probes and sweeps | Measure optical sensor response to help investigate scanning problems and evaluate firmware timing changes. |
| Scan interval and LED-duty measurements | Check scanning frequency and how long the sensor LEDs stay on. |
| Remote bootloader entry in diagnostic builds | Install another test image without pressing a physical reset button. |
| Optional automated key-event tests | Check actual key behavior and saved settings through injected presses, captured reports, and reboots. This is a diagnostic build option; see the [testing guide](../keytest.md). |

## A closer connection with Keybard

| Addition | Benefit |
| --- | --- |
| Board-provided layout and controls | Keybard reads the connected board’s layout, feature capacities, and available hardware controls. |
| Reads that skip empty feature slots | Loads populated behaviors without retrieving every unused entry. |
| Separate editor sessions | Keeps replies associated with the requesting connection and renews the session automatically. Use one editor at a time when changing settings. |
| Saved cluster selections | Keeps the selected finger and thumb cluster arrangement available for the editor’s layout view. |
| Active-layer queries and changes | Lets Keybard inspect and change the keyboard’s active layer. |
| Automatic default-layer reporting | Lets companion apps follow base-layout changes as well as active layers. Merged for builds after the original `vRC0`; existing Keybard layer reads remain compatible. |

The connection also supports companion applications that read the layout and follow live state. See [the protocol guide](protocol.md) for bidirectional communication, client coordination, and a preview of the key-peek trainer and app-aware layer work.

For technical details, source references, and known defects, see the [firmware review](../reviews/2026-10-04-qmk-fork-review.md). For everyday setup, start with the [Keybard feature guide](README.md).
