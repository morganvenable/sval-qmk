// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <stdbool.h>
#include <stdint.h>

void sval_mt_sample(const uint8_t *raw, bool valid);
void sval_mt_command(uint8_t *data, uint8_t length);
void sval_mt_task(void);
bool sval_mt_suppress_mouse(void); // the old single-pad injector's lease (mode 1)
uint16_t sval_mt_poll_ms(void);

void sval_mt_init(void);

bool sval_mt_suppress_pair_mouse(void); // unified mode: the pair-capture lease is live
uint8_t sval_mt_chip_filter(uint8_t pad, int16_t *h, int16_t *v);
uint8_t sval_mt_lease_mode(void);
uint8_t sval_mt_pad_contacts(uint8_t pad);

#ifdef POINTING_DEVICE_ENABLE
#    include "report.h"
// Both hands' chip reports ([left], [right]) after the unified-mode, claim and
// handoff rules: the chip wheel may pass while everything else is held back.
void sval_mt_filter_reports(report_mouse_t *left, report_mouse_t *right);
#endif

// Auto mouse from contact movement (keymaps/keymap_support.c), 800-DPI counts.
void sval_mt_contact_activity(bool left, uint16_t movement);
