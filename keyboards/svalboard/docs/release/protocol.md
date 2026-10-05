# A keyboard your apps can talk to

[Keybard feature guide](README.md) · [Firmware additions](firmware-changes.md)

Sval QMK gives desktop applications a two-way connection to your Svalboard. An app can read the layout stored on the keyboard, follow its active layers, inspect physical key activity, and send configuration or layer changes back. This is what makes Keybard possible—and it provides the foundation for tools that help you learn your layout or adapt it to the application you are using.

The keyboard still runs your saved layout itself. Host applications add editing, visual feedback, and context while they are connected; ordinary typing does not depend on keeping Keybard open.

## What travels in each direction

Normal key presses and pointer movement reach your computer through the usual USB keyboard and mouse interfaces. Alongside those, Sval provides a **Raw HID connection** for applications. Keybard uses that connection through the browser’s WebHID support; native companions can use a desktop HID library.

| From the keyboard to an app | What it enables |
| --- | --- |
| Board definition, matrix geometry, and selected cluster configuration | Draw the connected Svalboard with the appropriate physical positions. |
| Key assignments across all layers | Display or back up your actual layout directly from the board. |
| Programmable behaviors, including tap-dance actions | Show what a configured key can do and edit its actions. |
| Active-layer state | Update a layout display as you hold or switch layers. |
| Physical switch-matrix state | Highlight held keys for troubleshooting or a learning aid. |
| Feature capacities, settings, and hardware information | Load the board’s configuration and present its controls. |

| From an app to the keyboard | What it enables |
| --- | --- |
| Keymap and programmable-behavior changes | Edit assignments, shortcuts, and macros without compiling firmware. |
| Pointing and other supported hardware settings | Adjust the keyboard through Keybard. |
| Active-layer changes | Select an existing layer immediately, without rewriting its assignments. |
| Diagnostic requests | Ask for matrix state or use advanced scan diagnostics when needed. |

Configuration edits and live layer selection serve different purposes. Changing a key assignment updates the stored layout. Selecting an active layer changes runtime state: switching layers does not save another copy of the layout or cause a flash write for each application switch.

The current connection uses **requests and replies**. An app asks for state and receives a snapshot. Live displays poll for fresh snapshots; this release does not provide a continuous push stream of every key press and release. Matrix snapshots are useful for highlighting held keys, but a very brief tap can fall between polls.

## More than one app can participate

A layout editor and a trainer need different things from the same keyboard. Keybard changes assignments; a trainer reads those assignments and follows layer changes. Sval’s **client-ID wrapper** gives cooperating applications a way to distinguish their conversations.

An application first requests a client ID. Its requests carry that ID, and the keyboard echoes it in the replies. Each application accepts replies addressed to its own ID and ignores the others. IDs expire, so clients renew their connection identity as needed. The initial exchange also echoes a random nonce, letting an app recognize the reply to its own connection request.

That allows applications to share the protocol without mistaking another app’s response for their own. Actual simultaneous access also depends on the operating system and the applications’ HID implementations.

Client IDs route replies; they do not reserve settings or resolve competing edits. Two apps writing the same assignment can still overwrite each other. The useful arrangement is one editor, with other tools observing state or controlling an agreed part of runtime behavior. A trainer should reload its layout after you edit it: this release has no layout-change notification to refresh another app’s cached copy automatically.

## Coming into view: key-peek in the trainer

The trainer work uses this connection to put a reference to **your own layout** beside your work. Instead of consulting a static diagram, you can see the bindings for the layer you are using. Holding a navigation layer can reveal navigation keys; switching to symbols can reveal the corresponding symbols. Transparent positions resolve through the lower layers so the reference remains useful across a layered layout.

The developing trainer already reads the board’s layout and cluster selections, follows active-layer changes, and offers optional held-key highlighting through matrix snapshots. That is the foundation of the key-peek experience: glance at the keyboard overlay when you need a reminder, then keep working. Reading those definitions and states does not require capturing the text you type into other applications.

An overlay can describe configured tap and hold actions, but displaying a binding is different from observing which action the firmware ultimately executes. Richer feedback for resolved tap dances, combos, and other timed behaviors is a further step. Automatic default-layer reporting is also being developed as a capability extension; `vRC0` reports the active-layer mask, with the base/default choice supplied by the host tool.

These are previews of the trainer experience being built on the protocol, not a trainer bundled with the firmware download.

## Coming next: layers that follow your application

App-aware layer switching brings information in the other direction. Your computer knows which application has focus; a companion can use that information to select an appropriate keyboard layer.

For example, you could configure a CAD layer in Keybard and have it appear when you enter your CAD application, then return to your normal layout when you switch away. The shortcuts remain stored on the board. The companion chooses when to use them, so switching applications requires neither a reflash nor a new layout upload.

The existing layer-state command supplies basic host-controlled switching. The app-aware work in progress adds a **separate, temporary host layer** with behavior designed for everyday use:

- Manual non-base layers take priority, so holding your own navigation or symbol layer still works.
- Transparent keys in the app layer fall through to the normal base/default layers.
- Pausing the companion removes its layer contribution while preserving your manual layer state.
- A short, renewable lease removes the host contribution if the companion stops responding. The current draft uses a five-second lease.

A Windows companion draft matches foreground applications, with optional browser assistance for supported website contexts. The host supplies that context; the keyboard itself does not inspect running applications. The app-aware extension and companion are separate development work and are not included in `vRC0`.

Together, these directions make a useful loop possible: **the host chooses an application layer, the keyboard applies it, and a trainer shows the relevant bindings.** Completing that loop requires the trainer to understand the host-layer extension as well as ordinary manual layers. The extension keeps those states separate, so an older active-layer display alone will not show the full app-aware result.

## Under the hood

The release uses **Sval protocol version 3**, carried in 32-byte Raw HID reports. A client-ID envelope carries either familiar VIA operations, such as keymap reads and writes, or Sval-specific operations for programmable behaviors, board definitions, and live state.

| Protocol capability | Practical benefit |
| --- | --- |
| Board-provided compressed definition | Applications can load the keyboard’s geometry and configuration information directly. |
| 16-bit behavior indices | Address all 256 entries in each supported behavior table. |
| Version 3 sparse table reads | Retrieve populated behaviors without downloading every empty slot. |
| Version 3 macro transfers with 32-bit offsets | Access macro storage beyond the older 64 KiB addressing boundary. |
| 32-bit active-layer mask | Describe several active layers together, rather than only a single layer number. |
| Client IDs echoed in replies | Keep cooperating applications’ request/reply conversations distinguishable. |

App authors should serialize requests within each connection, check reply identity and command, renew client IDs, and recover cleanly from disconnects. Feature capabilities distinguish extensions that share a protocol version. A read-only companion can restrict itself to reads; an editor or layer controller can add only the writes its purpose requires.

For packet formats and implementation details, see the [client-ID protocol reference](../../../../modules/svalboard/core/docs/CLIENT_ID_PROTOCOL.md), [Sval command definitions](../../../../modules/svalboard/core/sval.h), and [command handlers](../../../../modules/svalboard/core/sval.c).
