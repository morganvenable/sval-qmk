# The Sval protocol

Svalboard QMK gives desktop apps a two-way connection to the keyboard over Raw HID, alongside its normal keyboard and mouse interfaces. Keybard uses it through the browser's WebHID; native apps can use any HID library. The keyboard runs its saved layout on its own, so no app has to stay open.

## What an app can do

**Read:** the board definition and geometry, every layer's key assignments, tap dances, combos, overrides, macros and the other behaviors, settings, active and default layers, and which physical keys are held.

**Write:** key assignments, behaviors, macros, pointing and other settings, and the active layer. Selecting a layer changes runtime state only; it doesn't rewrite the stored layout.

Host apps can read and control layer state.

## Sharing the keyboard between apps

Every message is a 32-byte report:

| `0xDD` | client ID (4 bytes) | `0xDF` Sval or `0xFE` VIA | payload |
| --- | --- | --- | --- |

An app asks for a client ID by sending a random 20-byte nonce with ID `0`. The keyboard echoes the nonce with a new ID and a lease of about two minutes. The app sends that ID with each request, the keyboard echoes it in the reply, and each app keeps only its own replies. Before the lease ends, the app asks for a new ID. The keyboard stores nothing per app, so a crashed app's ID simply expires.

Client IDs keep replies apart; they don't lock settings. Two apps editing the same thing can overwrite each other, so use one editor and let other tools read.

## Limits

- Apps poll for state; there is no push stream of key events, so a very short tap can fall between two polls.
- There is no notification when the layout changes. An app that caches the layout, such as a trainer, should reload after you edit.
- Whether two apps can open the keyboard at once depends on the operating system.

## Version 3

The launch firmware speaks Sval protocol version 3:
- 16-bit indices reach all 256 entries of each behavior table.
- Sparse reads skip empty entries.
- 32-bit macro offsets reach past 64 KiB.
- 32-bit layer masks report active and default layers.
- `GET_INFO` reports the protocol version, feature flags and the QMK keycode numbering the board uses.

Check feature flags before using an extension, send one request at a time per connection, and check the client ID and command in every reply.

## Pointing hardware

`POINTING_HARDWARE` (`0x2C`) tells a host which side each half thinks it is, which trackball sensor its firmware drives, and which sensor actually answered. PMW3360 and PMW3389 need different firmware, and the wrong one leaves the trackball dead with no other sign, so a host can use this to name the right `.uf2`.

- Discovery: GET_INFO byte 19 (the second feature byte), bit 1 (`sval_flag2_pointing_hardware`). Older firmware leaves it zero.
- Request: `DF 2C`
- Response: `DF 2C <format> <this half: 5 bytes> <other half: 5 bytes>`. `format` is `1`; `0` means the board has nothing to report. "This half" is the one plugged into USB.
- Each half: `<flags> <built_for> <detected> <Product_ID> <Inverse_Product_ID>`.
  - `flags`: bit 0 the block holds a report, bit 1 the half is the left half, bit 2 the sensor driver initialised.
  - `built_for`: `0` not a trackball build, `1` PMW3360, `2` PMW3389.
  - `detected`: `0` not checked, `1` PMW3360 (`42 BD`), `2` PMW3389 (`47 B8`), `3` no answer (`00 00` or `FF FF`), `4` some other ID.
- The other half reports through the split link's half-second presence ping, so its block is zero while it is disconnected, for up to half a second after it connects, and when it runs firmware without this command.

Packet formats: [client-ID protocol](../../../modules/svalboard/core/docs/CLIENT_ID_PROTOCOL.md), [layer state](../../../modules/svalboard/core/docs/LAYER_STATE_PROTOCOL.md), [command definitions](../../../modules/svalboard/core/sval.h).
