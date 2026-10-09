# Draft: release notes for the "last manual update" release

Draft for the first release built with the in-firmware updater (plan M3,
[updater.md](updater.md)). The tag name is not chosen yet: `<TAG>` below.
Fill in the other changes of that release where marked. Wording rules for
Svalboard release notes apply: describe additions relative to Svalboard's
Vial firmware, keep any safety claim to one plain line, and list the files
without naming sensor parts.

---

## <TAG>: the last manual update

This is the last time you need to copy firmware onto your Svalboard by hand.
From this release on, Keybard can install new firmware for you over USB.

### Install this release

1. Download the `.uf2` file for each half: one ending in `left`, one ending in
   `right`, for the pointing device each half has (none, trackball,
   TrackPoint or touchpad).
2. Put one half into bootloader mode (hold its BOOT button while plugging it
   in, or tap its reset button twice). A drive called `RPI-RP2` appears.
3. Copy that half's `.uf2` onto the drive. The half restarts on its own.
4. Do the same for the other half. **Both halves must run this release:** a
   half on this release cannot talk to a half on an older one.

### What's new compared with Svalboard's Vial firmware

- **Firmware updates from Keybard.** Keybard sends the new firmware over USB,
  the half checks it, and you approve it on the keyboard itself. Keybard
  updates the half without USB through the cable first, then the half with
  USB.
- **Approve on the keyboard.** When Keybard asks, hold Index South and Middle
  South together for one second on the half plugged into USB (on the default
  layout, `M` and `,` on the right, `V` and `C` on the left). The LEDs blink
  blue while it waits and flash white when it sees the keys.
- **Only Svalboard's firmware.** Each update is signed by Svalboard, and the
  keyboard refuses anything that isn't, or that was built for the other hand
  or a different pointing device.
- **Version check between halves.** If the two halves run different releases,
  Keybard tells you and the LEDs on the half with USB blink red until both
  match.
- `<other changes in this release>`

If an update is ever interrupted, the half starts in bootloader mode and you
copy its `.uf2` again as above.

### Files

- `*_left_*.uf2`, `*_right_*.uf2`: firmware for each half, for copying by hand.
- `*.svup`: the same firmware, signed, for Keybard's updater. You don't need to
  download these.

---

Notes for whoever publishes this (delete before publishing):

- Check the release's `.uf2` names against the list above before posting.
- Say whether keymaps and settings carry over from the release people are
  coming from; this draft makes no claim either way.
- The approval chord and LED colours are from `docs/updater.md` (D4, D24); if
  either changes before the release, update the text.
- The in-Keybard update flow is milestone M4 (Keybard UI). Until it ships, the
  "Firmware updates from Keybard" line describes a capability the firmware has
  and Keybard does not yet offer: hold the notes, or reword that line, until
  M4 is live on keybard.svalboard.com.
