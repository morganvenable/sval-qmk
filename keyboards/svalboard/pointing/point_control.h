// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "quantum.h"
#include "scroll_policy.h"

enum sval_point_keycodes {
    SV_POINT_L_VOLUME_HOLD = QK_KB_0, SV_POINT_L_VOLUME_TOGGLE,
    SV_POINT_L_ARROWS_HOLD, SV_POINT_L_ARROWS_TOGGLE,
    SV_POINT_L_TABS_HOLD, SV_POINT_L_TABS_TOGGLE,
    SV_POINT_L_TASKS_HOLD, SV_POINT_L_TASKS_TOGGLE,
    SV_POINT_R_VOLUME_HOLD, SV_POINT_R_VOLUME_TOGGLE,
    SV_POINT_R_ARROWS_HOLD, SV_POINT_R_ARROWS_TOGGLE,
    SV_POINT_R_TABS_HOLD, SV_POINT_R_TABS_TOGGLE,
    SV_POINT_R_TASKS_HOLD, SV_POINT_R_TASKS_TOGGLE,
    SV_POINT_CANCEL, SV_POINT_TRACE, SV_POINT_FINER, SV_POINT_COARSER
};
bool sval_point_process(uint16_t keycode, keyrecord_t *record);
bool sval_point_active(uint8_t side);
bool sval_point_blocked(uint8_t side);
void sval_point_buffer(uint8_t side, int16_t x, int16_t y, uint16_t cpi, uint16_t sniper_divisor);
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
