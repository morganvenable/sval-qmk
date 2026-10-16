// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "quantum.h"
#include "scroll_policy.h"

// The whole QK_KB block (0x7E00-0x7E3F, 64 codes) is reserved for the pointing tools.
// The codes are permanent: layouts store them. New keys go after the last one; never
// renumber or reuse a code. Codes in the block with no key yet do nothing.
//
// Slot keys are SV_POINT_FIRST + side * 8 + slot * 2 + mode: side 0 left pointer, 1 right;
// slot 0-3 (TOOL1-TOOL4); mode 0 hold, 1 toggle. A slot's name never says its action,
// which is a setting of the slot.
#define SV_POINT_FIRST QK_KB_0
#define SV_POINT_RESERVED_LAST (QK_KB_0 + 63)
enum sval_point_keycodes {
    SV_POINT_L_TOOL1_HOLD = SV_POINT_FIRST, // 0x7E00
    SV_POINT_L_TOOL1_TOGGLE,
    SV_POINT_L_TOOL2_HOLD,
    SV_POINT_L_TOOL2_TOGGLE,
    SV_POINT_L_TOOL3_HOLD,
    SV_POINT_L_TOOL3_TOGGLE,
    SV_POINT_L_TOOL4_HOLD,
    SV_POINT_L_TOOL4_TOGGLE,
    SV_POINT_R_TOOL1_HOLD, // 0x7E08
    SV_POINT_R_TOOL1_TOGGLE,
    SV_POINT_R_TOOL2_HOLD,
    SV_POINT_R_TOOL2_TOGGLE,
    SV_POINT_R_TOOL3_HOLD,
    SV_POINT_R_TOOL3_TOGGLE,
    SV_POINT_R_TOOL4_HOLD,
    SV_POINT_R_TOOL4_TOGGLE,
    SV_POINT_STOP_ALL, // 0x7E10: end every tool; an open task switcher is canceled
    SV_POINT_TRACE,    // 0x7E11: console counters (diagnostic)
    SV_POINT_FINER,    // 0x7E12
    SV_POINT_COARSER,  // 0x7E13
    SV_POINT_LAST = SV_POINT_COARSER,
};
_Static_assert(SV_POINT_L_TOOL1_HOLD == 0x7E00 && SV_POINT_R_TOOL1_HOLD == 0x7E08 && SV_POINT_STOP_ALL == 0x7E10 && SV_POINT_COARSER == 0x7E13, "pointing tools key codes are permanent");
_Static_assert(SV_POINT_LAST <= SV_POINT_RESERVED_LAST, "pointing tools keys stay inside the reserved QK_KB block");
bool sval_point_process(uint16_t keycode, keyrecord_t *record);
bool sval_point_active(uint8_t side);
bool sval_point_blocked(uint8_t side);
// Raw sensor counts, before sniper and boost: the tools ignore both.
void sval_point_buffer(uint8_t side, int16_t x, int16_t y, uint16_t cpi);
// The host is macOS (or iOS): Tabs, Tasks, Zoom and Undo / Redo send the Command shortcuts.
void sval_point_host_mac(bool mac);
void sval_point_discard_interval(void);
void sval_point_finish_interval(uint8_t axes);
void sval_point_policy_changed(bool enabled);
void sval_point_task(void);
void sval_point_mouse(report_mouse_t *report);

void sval_point_status(uint8_t *data);
void sval_point_stop(void);
void sval_point_tuning_changed(void);

// Relative-position scroll: output through the normal mouse path, not key taps.
bool sval_point_scroll_engaged(void);
void sval_point_scroll(report_mouse_t *report, uint8_t axes, bool natural, uint8_t host_divisor);
