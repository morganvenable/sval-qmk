// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <stdbool.h>
#include <stdint.h>

void sval_mt_sample(const uint8_t *raw, bool valid);
void sval_mt_command(uint8_t *data, uint8_t length);
void sval_mt_task(void);
bool sval_mt_suppress_mouse(void);
uint16_t sval_mt_poll_ms(void);

void sval_mt_init(void);
