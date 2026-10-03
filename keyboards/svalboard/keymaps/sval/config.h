/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

// Derive the USB serial from the chip's unique hardware ID instead of a fixed
// literal shared by every board. The module re-attaches the "sval:" magic as a
// prefix, so host detection is unchanged while the serial becomes unique per board
// and stable across reflashes. Measured: a serial that moves creates a new device
// instance on the host and costs the user their browser WebHID grant; a serial that
// holds still lets the product name change in place with the grant intact.
#undef SERIAL_NUMBER

#define STENO_COMBINEDMAP
