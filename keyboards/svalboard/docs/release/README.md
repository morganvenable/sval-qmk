(slop warning from claussen:  These release notes are AI-generated, with occasional human edits)

# Make Svalboard your own with Keybard

Keybard is the dedicated browser-based configuration tool for Svalboard, bringing visual layout design, programmable key behaviors, pointing controls, and hardware diagnostics together. Svalboard QMK runs your configuration on the keyboard, so your mappings, macros, and pointing settings keep working after you close the editor.

Explore the features below, or read [what Svalboard QMK adds to QMK](firmware-changes.md).

## The highlights

- **Design visually:** drag keys into place, compare layers, inspect transparent-key behavior, and choose a flat or 3D view.
- **Make every position do more:** tap dances, combos, macros, overrides, repeat mappings, and leader sequences are configurable without compiling firmware.
- **Build around your pointing devices:** independent pointer controls, automatic mouse-layer activation, precision Sniper keys, and faster Boost keys.
- **Try changes as you go:** apply edits live or queue them for review, reuse saved layers, and print a layout reference.
- **Keep your setup:** compatible firmware updates preserve settings; supported shipped Vial configurations migrate automatically; native layout files provide a portable backup.
- **Recognize your board:** a saved name and stable serial distinguish your keyboard and preserve its identity across updates.

## Start here

1. Open Keybard in a desktop browser with WebHID support and connect your Svalboard.
2. Choose your Svalboard when the browser asks for permission. Your layout and settings will appear in the editor.
3. Export a **`.svil`** backup before experimenting. This is the native format for the complete Svalboard setup.
4. Drag a key from a panel onto your layout, or select a position and type its new assignment. With Live Updating enabled, supported edits go straight to the board. With Manual Changes, use **Update** to apply queued edits or **Revert** to discard them.
5. Add a behavior in its editor, tune the pointers, or save a layer in the Layouts panel. Close Keybard when finished: the keyboard runs its saved configuration itself.

Use Chrome or Edge with WebHID enabled to connect to your board. Firefox and Safari cannot connect to the keyboard through Keybard. Layout files can be opened for editing without a connected keyboard, but hardware testing and applying changes require a device. Choose a language palette that matches your operating system’s keyboard layout so the characters you type match the labels you see.

## Design your layout

### A visual workspace for a 16-layer keyboard

| Capability | What you can do | Why it helps |
| --- | --- | --- |
| Drag-and-drop assignment | Place keys from the palettes; move or swap assignments, including between layers. | Try a new arrangement without editing C or memorizing numeric keycodes. |
| Typing Binds a Key | Select a position and press a key, including modifier combinations. Serial assignment can advance through positions. | Enter familiar shortcuts directly and fill an entire cluster efficiently. |
| Key palettes | Choose standard keys, modifiers, function/media/system keys, layer actions, mouse keys, and Svalboard controls. | Discover the available actions in one editor. Available actions depend on your firmware. |
| Multi-layer display | View several layers together; switch between flat and 3D presentations. | Compare related layers and see how a small physical keyboard becomes a larger working space. |
| Transparency visualization | Hide transparent layers or keys and inspect the underlying assignment; hide thumb keys when focusing on fingers. | Understand what a position does through layer fall-through. |
| Layer organization | Rename and color layers; copy, paste, blank, or make a layer transparent through its menu. | Give navigation, symbols, numbers, and application layers recognizable roles. |
| Named behaviors | Give macros and tap dances readable names in the editor and native backup. | Recognize an action by its purpose rather than its slot number. |
| Adjustable workspace | Use a sidebar or bottom panel, adjustable key sizes, and responsive cluster spacing. | Keep the keyboard and its editor usable on different screen sizes. |
| International palettes | Choose among the supplied language and layout palettes. | Pick the key labels and assignments appropriate to your host layout. This does not switch the OS layout for you. |

Your `.svil` backup includes layer names, behavior names, and other editor labels. Import that file when moving to another browser; these names do not currently synchronize automatically from the keyboard.

### Programmable behaviors without a firmware build

You have **256 slots each** for tap dances, combos, macros, key overrides, alternate-repeat mappings, and leader sequences. Macros share a total storage budget, so their lengths determine how much space remains.

| Feature | Behavior | Benefit and example |
| --- | --- | --- |
| Tap dance | Choose separate outputs for tap, hold, double-tap, and tap-then-hold. | Put related actions on one physical position: a symbol on tap and a different action on double-tap. |
| Combos | Turn a chord of up to four keys into another action; enable or disable each combo. | Add an ergonomic Escape, Tab, or shortcut without sacrificing a dedicated position. |
| Macros | Combine text, key presses, key releases, and delays, including layer actions and Svalboard controls. | Automate a frequently typed string or a repeatable sequence of keyboard actions. |
| Key overrides | Replace a key under selected modifier and layer conditions, with modifier masks and options. | Make Shift plus a chosen key produce a more useful symbol or shortcut. |
| Alternate repeat | Map a remembered key to an alternate output, with modifier conditions and enabled state. | Build common key pairs and editing patterns around QMK's Repeat/Alternate Repeat system. |
| Leaders | Configure an ordered sequence of up to five keys and an output keycode, with leader timing settings. | Make a mnemonic command sequence; the output can be a macro key. |
| One-shot/mod-tap composer | Combine left/right modifier choices visually, with MEH and HYPER presets, and assign the resulting one-shot or mod-tap key. | Enter modifier chords without holding several keys, or combine a tap action with a modifier hold. |
| Typing and mouse-key settings | Adjust leader timing, modifier swaps, simultaneous-key reporting (NKRO), and supported mouse-key controls. | Adapt shortcuts and keyboard-controlled pointer movement to your preferences. |

Some timing controls are not yet effective in this release. See [Current limitations](#current-limitations) before relying on changes to tap/hold, tap-dance, or combo timing.

### Pointing that fits the way you work

Use the **Pointing Devices** panel to tune each side of your Svalboard. Available controls depend on the sensors installed.

| Capability | Benefit |
| --- | --- |
| Independent left/right sensitivity and scroll configuration | Use one device for cursor movement and the other for scrolling, or tune each side to your preference. Available DPI choices depend on the sensor. |
| Per-pointer automouse participation | Choose which pointer activates the mouse layer, rather than having both sides trigger it indiscriminately. |
| Automouse threshold, decay, and timeout controls | Reduce unwanted layer activation and choose how the keyboard returns to typing. |
| Sniper keys, 2×/3×/5×, held or toggled | Slow cursor and scroll movement for precise placement without changing the normal DPI. |
| Boost keys, 2×/3×/5×, held or toggled | Move farther and faster when crossing a large desktop, then return to your normal speed. |
| Scroll hold/toggle, axis lock, and natural-scroll controls | Change between cursor and scroll work, avoid unwanted cross-axis scrolling, and select the direction that feels familiar. |
| Keyboard mouse buttons and movement/wheel keys | Keep clicks and pointer actions on the keyboard, including when a physical pointer is inconvenient. |
| TrackPoint recalibration | Recover from pointer drift on hardware that provides the recalibration operation. |
| Layer colors | Give the board's lighting a visible cue for the active layer. |

Hold a Sniper or Boost key for a temporary adjustment, or toggle it on for a longer task. Releasing a held key keeps an already-toggled mode active.

### Layouts you can keep, reuse, and learn

- **Layouts library:** browse bundled layers and your locally saved layers, preview them, search them, and drag a whole layer or an individual key into your working layout.
- **Reusable personal layers:** save a layer from its contextual menu, or import a layout file to use its layers as building blocks. Your personal library stays in this browser. Export a file to share it or move it to another computer.
- **Native `.svil` files:** export and import layouts, macros, supported dynamic behaviors, QMK settings, custom hardware values, cosmetic metadata, and fragment selections. Legacy `.viable` files remain readable; `.vil` is available for legacy exchange but cannot represent every Sval-specific field.
- **Import for your board:** bring a saved layout onto your connected Svalboard while retaining its hardware-specific limits.
- **Fragment composition:** select supported finger/thumb cluster fragments in Settings to make the drawing match the board's physical arrangement. Choose the clusters installed on your board.
- **Printed layers:** print non-empty layers through the browser, including saving a PDF where the browser offers it. A desk reference makes a new layout easier to learn.
- **Reconnect without another chooser:** Keybard lists matching devices the browser has already permitted, so you can reopen one directly while that permission remains available.
- **Version information:** check which version of Keybard you are using when reporting a problem.

Native exports are the portable backup. Browser libraries and presentation preferences depend on browser storage; the keyboard's serial and board name are stored separately on the board.

### Troubleshooting and advanced diagnostics

**Matrix Tester** shows which physical keys register. Press a key to see its position light up, making it easier to locate a key that needs attention.

**Scan Lab** provides advanced diagnostics for troubleshooting and firmware development, including status for both halves, optical scan timing measurements, and sensor-LED duty measurements. Normal keyboard setup does not require adjusting optical timing.

Idle power controls can dim lighting, put supported trackball sensors into rest modes, and reduce scanning and processor activity while the keyboard is unused. More aggressive power-saving settings can delay the first input after inactivity. Scan Lab displays estimated current consumption to help compare these settings.

## What Svalboard QMK adds

Svalboard QMK connects Keybard’s visual editor to the keyboard’s stored configuration. It builds on QMK’s key, layer, and programmable-action features with additions designed for Svalboard:

| Addition | What it gives you |
| --- | --- |
| Editable behavior tables | Change tap dances, combos, macros, overrides, repeat mappings, and leaders without rebuilding firmware. |
| Expanded storage | Keep up to 256 entries per feature, with more shared space for macros and settings. |
| Settings preservation | Keep a compatible setup across ordinary firmware updates. |
| Supported Vial migration | Bring an existing Svalboard configuration into the new firmware. |
| Persistent board identity | Give each board a recognizable name and keep its serial number through updates. |
| Efficient configuration reads | Load populated feature entries without waiting for every empty slot. |
| Integrated pointing and power controls | Tune both pointers, precision and speed modes, and idle behavior. |

[See all firmware additions and their benefits →](firmware-changes.md)

## Updating from Vial and choosing firmware

Automatic migration supports Svalboards running **`svalboard/vial-qmk v2025-11-01` with the `vial` keymap**. It imports the layout, macros, programmable behaviors, and supported settings. Export a backup first, and check your modifier preferences, timing settings, and macros after upgrading.

Other older firmware versions may start with default settings. Updates that change the storage layout can also require a reset, so keep an exported backup even when moving between Svalboard QMK versions.

Firmware downloads are available for the left and right sides of:

| Target family | Hardware |
| --- | --- |
| `svalboard/{left,right}` | Base board |
| `svalboard/trackball/pmw3360/{left,right}` | PMW3360 trackball |
| `svalboard/trackball/pmw3389/{left,right}` | PMW3389 trackball |
| `svalboard/trackpoint/{left,right}` | TrackPoint |
| `svalboard/azoteq/{left,right}` | Azoteq pointing hardware |

Choose the **sensor family and side** that match the half you are updating. Use the maintained **`sval`** keymap for the regular setup. Choose **`blank`** for an empty base-board layout. **`scanlab`** is intended for hardware diagnostics.

Enter the RP2040 bootloader by double-tapping reset within 500 ms. The half appears as **RPI-RP2**; copy its matching UF2 to that drive. See the [board README](../../readme.md) if you want to build your own firmware.

In Keybard's Settings, **Board name** accepts up to 32 characters within the firmware's 64-byte UTF-8 limit. Save it, then restart the keyboard for the computer to show the new USB product name. The serial stays the same. The name is stored on the board, separately from your layout backups.

## Current limitations

- **Timing controls:** changes to the general tapping term, per-dance tapping term, and per-combo timing currently save but do not change key behavior. Quick-tap and several tap/hold option controls also remain inactive. Flow Tap, Chordal Hold, and one-shot timing/locking adjustments are not available through these settings yet.
- **Auto Shift:** the standard firmware does not enable Auto Shift.
- **Editor names:** move layer and behavior names between browsers with a `.svil` export; they do not automatically load from the board.
- **Alternate repeat:** modifier conditions and the default-alternate option have known matching problems. Check the output of custom mappings before relying on them.
- **Editing held actions:** release a tap-dance key before editing its action to avoid leaving its previous output held.
- **After a reset:** restart the board before making new edits; edits made immediately after a configuration reset can otherwise be discarded at the next startup.

For the technical findings and test results, see the [firmware review](../reviews/2026-10-04-qmk-fork-review.md).
