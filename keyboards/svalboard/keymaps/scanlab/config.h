/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

// Scan Lab keymap: blank layout for matrix timing characterization. Nothing
// can be typed or clicked from this keymap, so a sweep at bad timing cannot
// reach the host. The USB product string is distinct so the board is easy to
// pick in a browser's device chooser.
#undef PRODUCT
#define PRODUCT "Svalboard ScanLab"

// Derive the USB serial from the chip's unique hardware ID instead of the
// module's fixed literal. Measured: the literal put every board on one Windows
// device instance, so two boards were indistinguishable and each firmware
// generation that changed the literal cost a WebHID re-grant. With the hardware
// ID the serial is stable across reflashes and unique per board.
// The module re-attaches the "sval:" magic as a prefix, so serial-based host
// detection still matches; keybard-ng matches on usage page 0xFF61/0x62 anyway.
#undef SERIAL_NUMBER

// Prototype boards without the revision strap: report revision B so the board
// boots on explicit conservative timing. Build with -DSVAL_HW_REV_FORCE=0 to
// test a revision A board, or remove this once the strap is fitted.
#ifndef SVAL_HW_REV_FORCE
#    define SVAL_HW_REV_FORCE 1
#endif

// Let the Scan Lab host reboot this board into the bootloader (two-stage
// arm/confirm) so development images can be flashed without touching it.
// The keyboard-level config defaults this to 0 and is included first.
#undef SVAL_HOST_BOOTLOADER
#define SVAL_HOST_BOOTLOADER 1
