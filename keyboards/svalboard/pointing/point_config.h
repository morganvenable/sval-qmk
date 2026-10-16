// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <stdbool.h>
#include <stdint.h>
#define SVAL_POINT_CONFIG_VERSION 5
#define SVAL_POINT_PROFILE_COUNT 4
enum { SVAL_TOOL_VOLUME, SVAL_TOOL_ARROWS, SVAL_TOOL_TABS, SVAL_TOOL_TASKS, SVAL_TOOL_ZOOM, SVAL_TOOL_HISTORY, SVAL_TOOL_HISTORY_Y, SVAL_TOOL_PROPORTIONAL, SVAL_TOOL_COUNT };
// For the proportional preset, reversal encodes dead-zone counts + 1 (0..63).
// Other presets keep its original reversal-tolerance meaning. The wire size stays fixed.
typedef struct {
    uint16_t distance, vertical_distance;
    uint8_t  interval_ms, reversal;
} sval_point_profile_t;
const sval_point_profile_t *sval_point_profile(uint8_t control);
bool                        sval_point_command(uint8_t *data, uint8_t length);
void                        sval_point_config_reload(void);

uint8_t sval_point_tool(uint8_t control);
// Travel per step is the slot's speed times the action's step scale. Tabs and Tasks take
// five times the travel of the other actions: one tab or window per step is a big jump, so
// at the default speed a step is 320 counts at 800 CPI, about 10 mm of ball travel.
#define SVAL_POINT_SWITCH_STEP_SCALE 5
uint8_t sval_point_step_scale(uint8_t tool);
// Tabs and Tasks with no rate limit set take this one: 140 ms, about 7 steps a second, the
// pace a host can show each tab or window and the user can stop on the one they want.
#define SVAL_POINT_SWITCH_INTERVAL_MS 140
// The slot's minimum interval between presses in ms (0: none), with that default applied.
uint16_t sval_point_interval(uint8_t control);
bool     sval_point_inverted(uint8_t control, uint8_t axis);
