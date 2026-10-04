# Experimental context-aware app layers

This Windows-first draft selects an existing configured layer from a companion
application. It keeps the automatic app contribution separate from QMK's manual
`layer_state` and persistent `default_layer_state`. It never writes a keymap or
changes saved defaults. Tap-dance variants are deferred and are not included in
this branch.

## Lookup and manual control

While an app context is active, each new key resolves in this order:

1. Active manual layers, highest number first, excluding layer 0 and layers in
   `default_layer_state` (these are base/default layers).
2. The selected app layer, regardless of its number.
3. The existing normal default/base fallback.

Transparent assignments fall through; `KC_NO` consumes the key. A held manual
layer therefore wins even when its layer number is lower than the app layer.
MO, TG, TO and tri-layer logic continue operating on manual state. Clearing or
expiring the app contribution cannot clear a manual layer, including when both
contributions name the same layer.

`TO(0)` and returning to a default layer do not pause app automation. Use the
companion's Pause/Resume controls for that. Selecting a nonbase manual layer via
TO takes priority over app assignments, but transparent positions still inherit
the app layer. This draft does not add an on-keyboard pause key.

The standard QMK source-layer cache retains the layer that produced each pressed
physical key, so release uses that layer even if the app changes or expires in
between. Automated selection takes effect immediately for subsequent lookups.
This build rejects `STRICT_LAYER_RELEASE` and `NO_ACTION_LAYER`, which disable
the required cache. Custom code that disables the action cache, keymap writes
during held keys, and complex synthetic gesture combinations need separate
hardware validation. A switch cannot preserve OS application focus or redirect
key releases back to an earlier window.

Normal `layer_state`, layer hooks, RGB layer indicators, standard Keybard active
layer displays, and layer-constrained feature logic see manual state only. The
companion reports the app layer separately. The diagnostic effective mask is a
union of contributions, not a numeric-priority representation of lookup order.

## Lease and wire protocol

`GET_INFO` feature bit 5 advertises support. Commands use the existing `0xDD`
client wrapper and `0xDF` inner protocol. Offsets below start at the inner `0xDF`
byte; all multi-byte fields are little-endian.

| Command | Request | Response |
|---|---|---|
| `0x26` set | bytes 2–3 nonzero transaction; byte 4 app layer | byte 2 status |
| `0x27` status | none | byte 2 status=0; byte 3 version=1; byte 4 layer count; byte 5 app layer or 255; bytes 6–7 transaction; bytes 8–9 lease milliseconds remaining; bytes 10–13 manual mask; bytes 14–17 default mask; bytes 18–21 effective union |
| `0x28` renew | bytes 2–3 current transaction | byte 2 status |
| `0x29` clear | none | byte 2 status=0 |

Status 0 means success; 1 means invalid layer/transaction. SET atomically replaces
the single app contribution and starts a five-second lease. Renew about once a
second. Expiry is enforced before every fresh key lookup and protocol command.
An expired transaction cannot be renewed; send SET to restore the desired context.
CLEAR, expiry, unplug, and reboot remove only the app contribution. Firmware
does not need a host cleanup message to stop using an expired contribution.

Use one companion per keyboard; this experimental protocol does not arbitrate
multiple hosts. The host should serialize HID requests, check acknowledgements,
and show device-reported status rather than just requested configuration.

## Validation and build

Run `python3 -m unittest discover -s tests/sval_storage -v`. The host C harness
compiles production protocol, layer resolver and source-cache functions with
undefined-behavior checking. It verifies manual priority independent of index,
same-layer ownership, transparent fallback, held-key release after switching,
invalid requests, renewal/expiry and timer wrap.

Build PMW3389 variants with:

```
make svalboard/trackball/pmw3389/left:sval
make svalboard/trackball/pmw3389/right:sval
```

The `context-draft.yml` workflow runs the layer regression test and produces both
UF2 artifacts. Use only the variant matching the physical hardware. This draft
has compiled and host-tested behavior; physical board validation is still needed.
