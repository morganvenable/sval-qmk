// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define SVAL_PERF_BUCKETS 6
#define SVAL_PERF_PAGE_WORDS 13
#define SVAL_PERF_PAGES 3

uint32_t sval_perf_now_us(void);
void     sval_perf_reset(void);
void     sval_perf_loop(void);
void     sval_perf_scan(uint32_t now_us);
void     sval_perf_i2c(uint32_t us, bool ok);
void     sval_perf_rpc(uint32_t us, bool ok, bool fresh);
bool     sval_perf_page(uint8_t page, uint32_t out[SVAL_PERF_PAGE_WORDS]);
