# Direct manipulation and explicit rates — version 3

This revision supersedes the burst scheduler below. The eight-step cap and 320 ms catch-up window have been removed. Motion is current input, not a request for a future animation. Per-axis intent may be emitted while its motion frame is current, but a new frame replaces old intent and a zero-motion sensor report cancels pending output. There is no replay of discarded motion later. Ordinary pointer movement and shared axis lock remain on the existing path.

Sensitivity still determines normalized travel per action. Optional per-tool action-rate limits are now visible in the main Control speeds panel, including its Shared defaults view. Each can be disabled, and **Remove all action rate limits** disables all four without changing sensitivity. Fresh configurations are uncapped. Existing v1/v2 rates are preserved visibly on upgrade, not silently reset. With a limit enabled, motion above its throughput is discarded rather than queued; with the limit disabled, USB and application throughput determine the effective maximum. This is not a promise of lossless arbitrarily fast input into a keyboard-tap interface.

The old fixed 20 ms press plus 20 ms gap has been removed. Ordered accepted USB reports pace the pulse, without an additional software dwell time; report acceptance and release retry remain separate from pointer movement. Housekeeping advances at most one owned output at a time, with round-robin axis arbitration. Zero motion invalidates unaccepted synthetic presses but never discards a required release of a press already accepted by transport. Raw samples still pass through the existing shared scroll-policy batch and its normal input latency.

Settings table version is 3 with the same 20-byte storage block and bit layout. Interval 0 means no configured delay; explicit intervals range from 4 to 250 ms. This corresponds to settings up to 250 actions/s, subject to transport/app throughput. Version 1 and 2 storage migrates without changing those users' sensitivity or selected intervals. New Keybard reads v1/v2/v3 but requires v3 firmware before writing new controls; old apps cannot accidentally write a v2 table over v3.

The reviewed BKB implementation at e743714790fc5fd9147268db40bba86f856f5dbb has no equivalent eight-step/320 ms policy. Its Volume/Tab modes accumulate movement until a threshold, tap once, and reset the accumulator, discarding overshoot. argos_keycode_tap uses ARGOS_TAP_CODE_DELAY = 10 ms. Those details informed comparison only; this implementation retains Svalboard's ownership and shared-policy model.

Tests cover removal of the burst cap, zero-motion cancellation without catch-up, uncapped throughput above the former ceiling, exact explicit-rate cadence, independent axes, direction changes, existing report ownership/retry tests, visible rate controls, and v2/v3 table read/write compatibility.

---

# Flick handling correction (same version-2 settings)

The original output scheduler kept only one pending action per pointer: multiple distance crossings in one report were collapsed immediately, later reports replaced that action, and free-axis motion could overwrite the other axis. Pending work also expired after 80 ms even when the task-switch cadence was 160 ms. A compiled regression test reproduced a four-step volume flick producing only one pulse.

The scheduler now keeps independent signed pending counts for each axis, with round-robin output. It preserves the configured output interval. Admission is bounded by the number of steps that can fit into the next 320 ms at that interval (including any remaining wait), capped at eight per pointer. Pending work expires after 320 ms without a fresh step. Extreme input above that capacity is still rate-limited; this does not claim lossless delivery of arbitrarily fast motion. Already admitted USB work retains its separate short acceptance timeout and reliable release behavior.

Direction reversal clears only that axis's pending work and unaccepted output; the other axis survives. A new stroke after quantizer idle discards pre-idle intent. Activation changes, stop, suspend and axis-policy changes continue to clear pending intent. Fresh activations no longer inherit the previous tool's output delay. Ordinary cursor movement, the shared axis policy, settings format, and GUI are unchanged.

Tests exercise actual accepted reports for burst counts, task timing, both free axes, direction reversal, huge-input tail bounds, stop, stale work and idle restart. Existing output ownership/retry and shared-policy regression tests still pass. Physical validation of this correction remains pending.

---

# Shared and per-tool pointing speeds — version 2

The non-modal **Control speeds** panel lives in Pointing Devices in the existing local Keybard preview at http://localhost:5198/. This version requires updating both PMW3389 halves with the matching images, then reconnecting. Assignments and the existing scrolling policy are unchanged.

## Interaction model

New configurations enable **Use shared speeds for every tool**. Shared defaults have separate horizontal and vertical speed sliders with **Link horizontal and vertical speeds** enabled. Moving either linked slider changes both. Linking previously separate axes adopts the horizontal value explicitly. Speeds increase left to right, from 0.0625× to 8× relative to 64 normalized counts per action, using 64 logarithmic positions. Sniper and existing pointer normalization still apply.

Turn universal speeds off to enable per-tool inheritance controls. Each tool initially follows shared defaults. Clear **Use shared defaults for [tool]** to start an override from the current shared pair; the tool can link or split its own axes. Switching between tabs preserves unsaved edits. Changing shared defaults immediately changes inherited effective values, while explicit overrides remain independent.

An alert icon marks tools whose effective speeds, axis-link setting, or advanced output limits differ from defaults. Universal mode temporarily suppresses speed overrides without deleting them, so toggling it back off restores them. Per-tool revert clears that tool's override and resets its output limits. **Revert all tools to defaults** clears every override, restores per-tool output limits, and enables universal speeds; it preserves the user's chosen shared speeds. Revert and slider changes remain drafts until Save/Apply, following Keybard's normal Live/Manual behavior.

Volume consumes vertical motion, Tabs/Tasks consume horizontal motion, and Arrows consumes both. The other axis is retained for consistent configuration and future tool extensions. The existing shared free-scroll/axis-lock policy filters motion before these independent axis distances are applied. Output rate and reversal guardrails remain per-tool; speed describes output per distance, not a bypass of output rate limits. Reversal tolerance is capped against the selected axis's distance at runtime.

## Persistence and migration

Version 2 retains exactly the original 20-byte reserved block at custom-config bytes 80–99; no existing settings, keymaps, macros, EEPROM boundaries or storage stamps move. The marker, version and CRC surround a 16-byte payload. Its 126 used bits are: shared horizontal/vertical speed indices (6 bits each), linked (1), universal (1), then four tool entries each containing horizontal/vertical indices (6 each), linked (1), inherit (1), interval in milliseconds (8), and reversal minus one (6). The final two bits must be zero.

The 64 firmware distances equal round(1024 × 2^(-index/9)); endpoints are 1024 and 8 counts. The host and firmware share a golden default packet test. GET/SET commands and generation guards stay the same, but table version is 2. An old app cannot write version 1 over the new table. New Keybard reads either version, shows a firmware update notice for an old connected board, and prevents saving independent speeds until it is updated.

A valid v1 stored table migrates in RAM without a boot-time write. Each old distance becomes the nearest logarithmic speed position (up to about 4% rounding), linked on both axes, preserving individual behavior rather than silently replacing it with universal speeds. Intervals and reversal tolerances are retained exactly. Values matching the shared 64-count baseline inherit; other tools become overrides. Saving persists v2. Old exported layouts migrate on import to v2 boards; v2 imports to old boards are rejected during preflight. CRC failure defaults only pointing configuration, never unrelated settings.

## Validation

Tests cover shared and separate axes through the actual controller, inheritance and overrides, linked-pair validation, migration, persistence, CRC recovery, generation conflicts, active-control rejection, failed writes, prior-setting byte preservation, host/firmware packet parity, UI reset behavior, navigation without losing drafts, and Live/Manual queueing. Both PMW3389 half images are built. The user confirmed the original control modes work on hardware; this speed extension still needs physical validation.

---

The previous version's details follow for historical context. Version 2 supersedes its table format and motion-distance editor.

# Integrated Keybard pointing preview

This iteration is a system: opt-in QMK firmware and a matching local Keybard branch. It targets two PMW3389 pointers. No production deployment or upstream push is part of this preview.

## Trying it

Open the local Keybard preview at http://localhost:5198/. Connect and export your existing layout before updating firmware. In Pointing Devices, download and flash the matching left and right PMW3389 images to their respective halves, then reconnect. Select a physical key in your layout, open Pointing Devices, select Left pointer or Right pointer, and assign Volume, Arrows, Tabs, or Tasks as a hold or toggle. These assignments use the normal Live / Manual Apply flow.

Control feel edits distance per output, maximum output rate, and reversal tolerance for each preset. Save explicitly; Manual mode queues the change for Apply. Stop active controls before saving. Stop all controls cancels both pointers immediately. Profile tuning survives reboot on the USB-connected half and travels with layout exports. It is not mirrored between halves; moving the USB connection requires loading the same layout/tuning on the other half.

Ordinary scroll and discrete controls share the existing free-scroll/axis-lock policy, including its asymmetric thresholds and idle release. There is no independently configurable second axis lock. The architecture and controller details remain in discrete-pointing.md.

Tasks uses Windows Alt+Tab. Release or Enter accepts; Escape cancels; unrelated typing accepts before forwarding. Held physical modifiers pause synthetic output in this first pass. This is experimental firmware; builds and software tests do not substitute for physical validation.

## Firmware / Keybard contract

GET_INFO feature_flags2 bit 1 advertises version 1 pointing controls. Keybard only reads these commands when advertised; older firmware displays the matching firmware download instructions. Codes 0x7e00–0x7e13 have stable SV_POINT_* names, separate from existing Sval custom keycodes at 0x7e40 onward. Hold/toggle assignment is limited to direct physical key bindings in the pointing picker.

All commands use the existing client-ID-wrapped Sval protocol (0xDF). GET 0x2C returns status, version, uint16 generation and four profiles in Volume/Arrows/Tabs/Tasks order. SET 0x2D receives version, expected uint16 generation and all four profiles, and returns the same table after saving. A profile is distance LE16, interval uint8 milliseconds, reversal uint8 counts. Distances are normalized to 800 CPI. Valid ranges: distance 8–1024; interval 40–250; reversal 1–64 and no greater than distance. Status codes: 0 success, 1 active controls, 2 generation conflict, 3 invalid, 4 storage failure. The application reports refused saves through its existing changes queue. Reconnect reloads the table/generation after another client changes it.

STATUS 0x2E returns version, left/right active presets, shared axis mask, policy-enabled flag, task-session flag, temporary distance shift, then three uint32 LE counters (recognized, admitted, dropped). STOP 0x2F cancels all controls and acknowledges status/version. Status is read every 750 ms while the panel is visible. Stop is immediate rather than queued behind Manual Apply.

The table occupies bytes 80–99 of the existing 100-byte VIA custom-config reservation. Compile-time checks ensure existing settings plus their marker end before offset 80. The twenty bytes contain a two-byte marker/version, sixteen profile bytes, and CRC16-CCITT. Invalid data defaults only this section in RAM; it never resets unrelated EEPROM settings or writes defaults on boot. Saves validate everything, reject stale generations and active controls, write one complete block through existing wear leveling, and verify readback. This is not a dual-bank transaction: power-loss corruption falls back to profile defaults using CRC. Saving resets temporary distance adjustments and pending motion.

Keybard saves the whole profile table through ChangesContext under one coalescing write key, observes connection-session guards, includes tuning and stable key names in .svil exports, and rejects importing tuning to unsupported firmware. Sidebar, bottom bar and the assignment picker share MouseKeysSection and its key sizing.

## Validation

Compiled C tests cover defaults, persistence/readback, stale/active/invalid saves, storage failure, corruption recovery and preservation of prior settings bytes, alongside motion, shared policy, controller and USB-output tests. Keybard tests cover wire layout, errors, key names, file round trips, import queuing, tuning, immediate stop and both key layouts. Both PMW3389 half images are compiled. Physical validation remains outstanding.
