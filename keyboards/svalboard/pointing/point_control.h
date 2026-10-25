// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "quantum.h"
#include "scroll_policy.h"
#include "point_device.h"

// The whole QK_KB block (0x7E00-0x7E3F, 64 codes) is reserved for the pointing tools.
// Layouts store the codes: new keys go in free codes, and a retired code is never reused.
// Codes in the block with no key do nothing.
//
// Tools aren't left or right: a tool key turns every pointing module into that tool while
// it is held or toggled on, so whichever pointer moves (both, if both move) runs it. Tool
// keys are SV_POINT_FIRST + slot * 2 + mode: slot 0-3 (TOOL1-TOOL4); mode 0 hold, 1 toggle.
// A key's name never says its action, which is a setting of the slot.
//
// Retired codes, kept reserved: 0x7E08-0x7E0F (the right pointer's slot keys in the
// earliest test builds) and 0x7E12 / 0x7E13 (Finer / Coarser). Travel per step is set only
// with the tool speeds.
#define SV_POINT_FIRST QK_KB_0
#define SV_POINT_RESERVED_LAST (QK_KB_0 + 63)
enum sval_point_keycodes {
    SV_POINT_TOOL1_HOLD = SV_POINT_FIRST, // 0x7E00
    SV_POINT_TOOL1_TOGGLE,
    SV_POINT_TOOL2_HOLD,
    SV_POINT_TOOL2_TOGGLE,
    SV_POINT_TOOL3_HOLD,
    SV_POINT_TOOL3_TOGGLE,
    SV_POINT_TOOL4_HOLD,
    SV_POINT_TOOL4_TOGGLE,                   // 0x7E07
    SV_POINT_STOP_ALL = SV_POINT_FIRST + 16, // 0x7E10: end every tool; an open task switcher is canceled
    SV_POINT_TRACE,                          // 0x7E11: console counters (diagnostic, not offered by Keybard)
    SV_POINT_LAST = SV_POINT_TRACE,
};
_Static_assert(SV_POINT_TOOL1_HOLD == 0x7E00 && SV_POINT_TOOL4_TOGGLE == 0x7E07 && SV_POINT_STOP_ALL == 0x7E10 && SV_POINT_TRACE == 0x7E11, "pointing tools key codes are permanent");
_Static_assert(SV_POINT_LAST <= SV_POINT_RESERVED_LAST, "pointing tools keys stay inside the reserved QK_KB block");
bool sval_point_process(uint16_t keycode, keyrecord_t *record);
// A pointer runs the active tool (every pointer does while one is on).
bool sval_point_active(uint8_t side);
bool sval_point_blocked(uint8_t side);
// Raw sensor counts, before sniper and boost: the tools ignore both.
void sval_point_buffer(uint8_t side, int16_t x, int16_t y, uint16_t cpi); // a trackball at cpi
void sval_point_buffer_scaled(uint8_t side, int16_t x, int16_t y, sval_point_scale_t scale);
// A touchpad read from its contacts: whether a finger is down on it. Lifting to reposition
// is not motion: it never counts as a reversal and never ends a step in progress, and
// Proportional scroll stops and starts again from where the finger lands.
void sval_point_contact(uint8_t side, bool down);
// Any tool on (every pointer runs it).
bool sval_point_any_active(void);
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
