// Copyright 2026 Svalboard
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// Dev builds only (SVAL_TOUCH_DEV): injected key presses (key_inject.c).
#include <stdbool.h>
#include <stdint.h>
#include "matrix.h"

void sval_key_inject_apply(matrix_row_t *rows);                                           // matrix.c, this hand's raw rows
bool sval_key_inject_rpc(uint8_t in_len, const uint8_t *in, uint8_t out_len, uint8_t *out); // the other half's side
void sval_key_inject_command(uint8_t *data, uint8_t length);                              // Raw HID 0xFC op 0x44
