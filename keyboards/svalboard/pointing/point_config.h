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
// Every action has its own default step and pace (point_config.c, docs/pointing-tools.md).
// travel_x2 is twice the step's multiple of the slot's speed: at the default speed (64
// counts at 800 CPI, about 2 mm of ball travel) a step is 64 * travel_x2 / 2 counts.
// interval_ms is the minimum time between presses when the slot's rate limit is 0.
// Proportional scroll is continuous and uses neither.
typedef struct {
    uint8_t travel_x2, interval_ms;
} sval_point_action_default_t;
const sval_point_action_default_t *sval_point_action_default(uint8_t tool);
// Counts of ball travel at 800 CPI for one step of the action, from the slot's distance.
int32_t sval_point_step_counts(uint8_t tool, uint16_t distance);
// The slot's minimum interval between presses in ms (0: none): its rate limit, or the
// action's default pace when that is 0.
uint16_t sval_point_interval(uint8_t control);
bool     sval_point_inverted(uint8_t control, uint8_t axis);
