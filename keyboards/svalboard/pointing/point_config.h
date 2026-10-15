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
bool    sval_point_inverted(uint8_t control, uint8_t axis);
