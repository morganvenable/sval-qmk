// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <stdint.h>

#define KEYTEST_CHANNEL 0x54
#ifdef SVAL_KEYTEST
void keytest_command(uint8_t *data, uint8_t length);
void keytest_task(void);
#endif
