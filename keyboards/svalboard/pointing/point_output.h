// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "report.h"

typedef enum { SVAL_POINT_KEY, SVAL_POINT_CONSUMER, SVAL_POINT_TASK } sval_point_output_kind_t;
// A single owned contribution; never writes QMK's real/weak modifier bitmasks.
bool sval_point_output_start(sval_point_output_kind_t kind, uint16_t usage, uint8_t mods, uint32_t now);
void sval_point_output_task(uint32_t now);
void sval_point_output_end(bool cancel);
void sval_point_output_drop_unaccepted(void);
bool sval_point_output_session(void);
bool sval_point_output_interrupted(void);
void sval_point_output_reset(void);
void sval_point_output_observe_keyboard(const report_keyboard_t *report);
void sval_point_output_observe_nkro(const report_nkro_t *report);
void sval_point_output_observe_extra(const report_extra_t *report);
// Implemented in point_transport.c on the ChibiOS endpoints; false means retry.
bool sval_point_transport_keyboard(report_keyboard_t *report);
bool sval_point_transport_nkro(report_nkro_t *report);
bool sval_point_transport_extra(report_extra_t *report);
// Wraps the host driver so QMK's keyboard, NKRO and extra reports reach the broker.
void sval_point_transport_install(void);
