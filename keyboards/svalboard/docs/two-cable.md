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
| 0x02 LEAD | lead | seq, side, rows[5], dx, dy (int16), h, v (int8), buttons, flags (bit 0: reply) | `0x82` only if asked |
| 0x03 RELEASE | either | none: end the role | `0x83` |
| 0x04 STATUS | either | none | `0x84` |
| 0x05 INJECT | either | bench: rows[5], dx, dy | `0x85` |
| 0x06 CAPTURE | lead | bench: 1 on / keep alive, 0 off | `0x86` |
| 0x07 REBOOT | either | bench: leave USB and come back | `0x87` |

Replies: `[0x80|op][seq][status][role][side][capture][lead packets:4][time us:4]`.
Status: 0 OK, 1 not a follower (INJECT), 2 unknown op, 3 the split cable
works (FOLLOW and LEAD refused, and LEAD replies even when not asked).

Pushes from the board:

- `0x90` rows (follower): seq, side, rows[5], dx, dy, h, v, buttons, time us, flags.
- `0x92` applied (lead): seq, layer state, default layer state, mods, weak mods,
  one-shot mods, time us. Sent once the keymap has handled new rows.
- `0x93` report (lead, while capturing): kind (1 keyboard, 2 NKRO, 3 mouse,
  4 extra), time us, length, bytes.

The bench ops exist only for hardware tests: CAPTURE keeps every report the
lead makes off the computer and drains for a second after it ends, so a key
pressed by INJECT never types.
