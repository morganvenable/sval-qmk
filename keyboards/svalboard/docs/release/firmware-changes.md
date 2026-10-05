# What Svalboard QMK adds beyond Vial

[Explore Keybard](README.md) · [Release announcement](announcement.md) · [Protocol and companion apps](protocol.md)

If your Svalboard ran the Vial firmware (`svalboard/vial-qmk v2025-11-01`), you could already edit your keymap, tap dances, combos, key overrides, alternate-repeat keys, macros and QMK settings without rebuilding firmware. You also had layer colors, per-side DPI and scroll toggles, axis lock, Sniper hold keys, the automouse toggle and TrackPoint recalibration. All of that carries over. This page covers what Svalboard QMK adds on top.

## More room, and leader sequences

| Addition | Compared with Vial |
| --- | --- |
| 256 entries each for tap dances, combos, key overrides, alternate-repeat keys and macros | Vial allowed 50 tap dances, 50 combos, 30 key overrides and 50 macros. |
| Leader sequences | New: assign an action to an ordered sequence of up to five keys, editable in Keybard. |
| Larger macro storage | Macros can use the shared space beyond the previous 64 KiB boundary. How much is left for macros depends on the other stored features. |

Several timing controls currently save but have no effect. See [Current limitations](README.md#current-limitations).

## Your setup survives updates

| Addition | Compared with Vial |
| --- | --- |
| Settings kept across ordinary updates | Vial reset the layout whenever you installed a firmware build from a different day. A compatible layout now stays. |
| Automatic migration from Vial | Imports your existing layout, macros, programmable behaviors, pointing preferences and other supported settings on first boot. The old Vial store is left intact. |
| Keycode translation | Stored keycodes, including those inside tap dances, combos, overrides, leaders and macros, are translated when QMK's keycode numbering changes, instead of being reset. |
| Interrupted-migration recovery | An unfinished migration retries from the old configuration; a completed one doesn't repeat after a later reset. |
| Checked macro uploads | Uploads that exceed the macro buffer or contain invalid offsets are rejected. |

Automatic migration supports **`svalboard/vial-qmk v2025-11-01` with the `vial` keymap**. Keep an exported backup and check your modifier preferences, timing settings and macros after upgrading. Changes to the storage layout can still require a reset.

## A board that keeps its identity

| Addition | Benefit |
| --- | --- |
| Persistent serial number | Each board stays identifiable across firmware updates and settings resets. |
| Your own board name | Name each Svalboard so several are easy to tell apart in a device list. Save the name in Keybard, then restart the board. |

The name and serial are stored on the keyboard, separately from your layout backups.

## Pointing

| Addition | Compared with Vial |
| --- | --- |
| Sniper toggle | Sniper 2×/3×/5× can now be toggled as well as held. |
| Boost keys | New: hold or toggle 2×, 3× or 5× faster movement for crossing a large desktop. |
| Per-pointer automouse | Choose which pointer activates the mouse layer, instead of both. |
| Automouse threshold and decay | Tune how much movement activates the mouse layer and how it winds down; the timeout was already adjustable. |
| Natural scrolling | New: choose the scroll direction. |

## Lower power when idle

| Addition | Benefit |
| --- | --- |
| Light- and deep-idle scan rates | Scan less often during quiet periods. |
| Trackball rest modes | Let supported sensors use their own low-power modes. |
| Idle lighting and processor sleep | Dim the lighting and reduce processor activity between scans. |
| Selectable deep-idle clock | Further savings, at the cost of a possible delay on the first input after inactivity. |

Scan Lab shows estimated current draw for comparing settings.

## Hardware revision B

Boards with the revised analog front end ("flipfet") are detected automatically at startup and use scan timing measured for that hardware.

## Diagnostics and testing

These support troubleshooting and firmware development; normal setup doesn't need them.

| Addition | Purpose |
| --- | --- |
| Scan Lab | Measures optical scan timing, sensor-LED duty and idle current, and reports the status of both halves. |
| Remote bootloader entry in diagnostic builds | Install another test image without pressing reset. |
| Optional key-event tests | Check real key behavior and saved settings with injected presses and captured reports. See the [testing guide](../keytest.md). |

## For Keybard and companion apps

| Addition | Benefit |
| --- | --- |
| Sparse table reads | Keybard loads only the entries in use instead of every empty slot. |
| Client sessions | Several apps can talk to the board at once and each gets its own replies. Use one editor at a time when changing settings. |
| Saved cluster selections | Keybard draws your finger and thumb cluster arrangement. |
| Host access to layer state | Apps can read the active and default layers and control which layer is active. |
| Layer colors in Keybard | Choose each layer's lighting color in the editor; Vial had layer colors but no way to set them from the GUI. |

See [the protocol guide](protocol.md) for details.

For technical details, source references and known defects, see the [firmware review](../reviews/2026-10-04-qmk-fork-review.md). For everyday setup, start with the [Keybard feature guide](README.md).
