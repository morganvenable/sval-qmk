# Two halves on separate USB cables (prototype)

Build with `SVAL_TWO_CABLE=yes` (dev builds only; release builds refuse it).
Each half is plugged into the computer by its own USB cable, with no split
cable. A host relay (Keybard repo, `companion/two-cable-relay/`) stands in for
the split link:

- The **follower** stops typing. It pushes its debounced key rows and its
  pointer motion to the relay on every change, and every 100 ms otherwise.
- The **lead** takes those rows as the other half's rows, in the same place
  the split link puts them (`matrix[]`, the other hand's rows), and runs the
  keymap for both halves. Its pointing code gets the follower's motion as the
  other hand's report.

So layers, modifiers, combos, tap-hold, tap dance and the pointing modes work
across the halves as they do over the split cable. Only the transport differs.
The lead's same-side check also runs: the follower's side rides in every
packet.

## Leases

Both roles are held by the relay's packets. If the relay stops:

- the lead lets go of every relayed key after 500 ms;
- the follower types on its own again after 600 ms, like any half alone on USB.

If the relay sees a half leave USB, it acts at once: the lead's relayed keys
are released, or the follower is told to type on its own.

When the lead's role ends, the relayed rows go to zero first, and the other
half counts as gone (side unknown) only after a whole `keyboard_task()` pass
has released those keys. While the other half is gone, `side.c` drops events
from its rows, releases included, so the other order would leave a held
layer or tap-hold key on.

## What the lead reports for the other half

Over the cable, the other half's presence reply gives the master its pointing
module and main board, and Sval `0x2C`, `0x2D`, `0x18` (pointing fragments)
and `0x38` report them for that hand. With two cables the relay stands in:
about once a second it reads the follower's own `0x2C` and `0x38` blocks and
hands them to the lead with PEER. The lead uses them wherever the presence
reply's would go, while the split link is down and it leads; they are
dropped when its role ends. So Keybard and Keybard Overlay draw both hands'
pointing modules, and `0x2D` carries the follower's motion in its hand's
block (which needs that hand's identity, with the sensor started).

Still empty for the other half in two-cable mode, since each needs the split
link: the flicker latch (`0x3D`, Matrix Tester), Scan Lab, `0x33` SET and
CLEAR for the other half, the updater's split presence, and `0x2F` contacts.

## Link light

Two cables use the same link light as the split cable (`link_light.h`,
`docs/link-light.md`): amber, 2 Hz, on both halves.

- Taking a role (FOLLOW or LEAD) pairs a half.
- A lease that runs out (the relay crashed or hung) loses the pairing, on both
  halves.
- A RELEASE with bit 0 of its first byte set also loses it. The relay sends
  that when the other half left USB or stopped answering, so the half that is
  left blinks.
- A plain RELEASE (the relay stopped, or was uninstalled) is a stop on purpose:
  no light.
- The pairing comes back with the next role.

A half on its own from power-up never blinks. The updater's lights win, and the
light is dark in USB suspend.

## Sides

Each half needs a side, as over the cable (`side.h`). A half with no side
types nothing, and a lead whose follower reports no side, or its own side,
lets none of the follower's keys act. The relay checks both halves' sides
(Sval `0x33` GET) before it starts, and sets them when they are missing or
the same (`0x33` SET, source HOST, with a restart), after asking the person
to press a key on the left half: both halves follow for that moment, and the
one whose rows change is the left. The record it writes is the ordinary side
record, so normal firmware keeps it afterwards.

## The split cable

The relay only stands in for a split link that is down. With the link up
(`sval_other_half_connected()` on the half with USB), FOLLOW and LEAD are
refused with status 3 and a live role ends, so the cable and the relay never
both write the other half's rows. With no cable the split code never touches
those rows: `matrix_post_scan()` clears them only when a link that was up
goes down, and the time-based disconnect check (`SPLIT_MIN_DISCONNECT_MS`)
then throttles the master's attempts to one every 500 ms, each limited by the
3 ms per-scan deadline of the DMA receive.

## Wire

64-byte raw HID reports in the client wrapper: `[0xDD][client:4][0xFB][op]`.

| op | to | payload | reply |
|---|---|---|---|
| 0x01 FOLLOW | follower | none; renews the lease | `0x81`, then a rows push |
| 0x02 LEAD | lead | seq, side, rows[5], dx, dy (int16), h, v (int8), buttons, flags (bit 0: reply; bit 1: the rows hold injected keys, which act only while the lead captures) | `0x82` only if asked |
| 0x03 RELEASE | either | flags: bit 0 the other half was lost (the link light shows), else a stop on purpose | `0x83` |
| 0x04 STATUS | either | none | `0x84` |
| 0x05 INJECT | either | bench: rows[5], dx, dy | `0x85` |
| 0x06 CAPTURE | lead | bench: 1 on / keep alive, 0 off | `0x86` |
| 0x07 REBOOT | either | bench: leave USB and come back | `0x87` |
| 0x08 PEER | lead | held (bit 0 pointer, bit 1 board), the follower's own Sval `0x2C` block (5 bytes), its own `0x38` block (5 bytes) | `0x88` (byte 7 echoes the held byte, not a sequence) |

Replies: `[0x80|op][seq][status][role][side][capture][lead packets:4][time us:4]
[link light: bit 0 lost, bit 1 showing, bit 2 armed][PEER held: bit 0 pointer, bit 1 board][ms since the loss:2]`.
Status: 0 OK, 1 not a follower (INJECT), 2 unknown op (also the bench ops
without `SVAL_TWO_CABLE_BENCH`), 3 the split cable works (FOLLOW and LEAD
refused, and LEAD replies even when not asked), 4 INJECT on a lead that isn't
capturing, 5 PEER on a half that isn't leading.

Pushes from the board:

- `0x90` rows (follower): seq, side, rows[5], dx, dy, h, v, buttons, time us, flags.
- `0x92` applied (lead): seq, layer state, default layer state, mods, weak mods,
  one-shot mods, time us. Sent once the keymap has handled new rows.
- `0x93` report (lead, while capturing): kind (1 keyboard, 2 NKRO, 3 mouse,
  4 extra), time us, length, bytes.

The bench ops exist only for hardware tests, and only in builds with
`SVAL_TWO_CABLE_BENCH=yes`: CAPTURE hands every report the lead makes to the
client that asked, which no other app on the computer should be able to do.
CAPTURE keeps those reports off the computer and drains for a second after it
ends, and injected keys act only into a capture, so they never type.
