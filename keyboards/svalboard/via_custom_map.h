// SPDX-License-Identifier: GPL-2.0-or-later
// Byte ranges in the VIA custom config block (via_read_custom_config()). Each user owns
// its range whether or not it is built in, so builds with and without an experiment
// agree on every address. Add new ranges at the end; the checks below keep them apart.
// Changing VIA_EEPROM_CUSTOM_CONFIG_SIZE changes the VIA layout stamp (quantum/via.c), so
// the keymap and macros reset once on the first boot of such a build.
#pragma once

// Board settings: saved_values_t, then its 6-byte magic (svalboard.c).
#define SVAL_VIA_BOARD_OFFSET 0
#define SVAL_VIA_BOARD_SIZE 100
// Pointing tools speed record (pointing/point_config.c).
#define SVAL_VIA_POINT_TOOLS_OFFSET 100
#define SVAL_VIA_POINT_TOOLS_SIZE 20
// Touch gesture experiment (its block is 22 bytes; two spare).
#define SVAL_VIA_TOUCH_GESTURES_OFFSET 120
#define SVAL_VIA_TOUCH_GESTURES_SIZE 24
// Pointing tools device gains (pointing/point_config.c): touchpad and TrackPoint.
#define SVAL_VIA_POINT_GAINS_OFFSET 144
#define SVAL_VIA_POINT_GAINS_SIZE 8
#define SVAL_VIA_CUSTOM_SIZE 152

#if SVAL_VIA_BOARD_OFFSET + SVAL_VIA_BOARD_SIZE > SVAL_VIA_POINT_TOOLS_OFFSET
#    error "VIA custom config: board settings overlap the pointing tools"
#endif
#if SVAL_VIA_POINT_TOOLS_OFFSET + SVAL_VIA_POINT_TOOLS_SIZE > SVAL_VIA_TOUCH_GESTURES_OFFSET
#    error "VIA custom config: pointing tools overlap the touch gestures"
#endif
#if SVAL_VIA_TOUCH_GESTURES_OFFSET + SVAL_VIA_TOUCH_GESTURES_SIZE > SVAL_VIA_POINT_GAINS_OFFSET
#    error "VIA custom config: touch gestures overlap the pointing tools gains"
#endif
#if SVAL_VIA_POINT_GAINS_OFFSET + SVAL_VIA_POINT_GAINS_SIZE > SVAL_VIA_CUSTOM_SIZE
#    error "VIA custom config: pointing tools gains run past the block"
#endif
