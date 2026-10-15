// SPDX-License-Identifier: GPL-2.0-or-later
// Timing counters (SVAL_PERF_STATS, dev builds only); see perf.c.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define SVAL_PERF_BUCKETS 6
#define SVAL_PERF_PAGE_WORDS 13
#define SVAL_PERF_PAGES 5
#define SVAL_PERF_MATRIX_ROWS 5 // rows per half (matrix.c)
// The other half's counters ride on the Scan Lab RPC (KEYBOARD_SYNC_B) with
// this op, like the touchpad's (azoteq/multitouch.h): [op][page or 0x80
// reset][part] -> [op][ok][page][part][7 words].
#define SVAL_PERF_RPC_OP 0xAE
#define SVAL_PERF_RPC_REPLY 32

uint32_t sval_perf_now_us(void);
void     sval_perf_reset(void);
void     sval_perf_loop(void);
void     sval_perf_scan(uint32_t now_us);
void     sval_perf_i2c(uint32_t us, bool ok);
void     sval_perf_rpc(uint32_t us, bool ok, bool fresh);
void     sval_perf_rpc_hint(void); // a fetch the other half's news asked for
bool     sval_perf_page(uint8_t page, uint32_t out[SVAL_PERF_PAGE_WORDS]);
// Where the main loop's time goes: microseconds spent in a section.
enum { SVAL_PERF_SEC_TOUCH_TASK, SVAL_PERF_SEC_PAD_CALL, SVAL_PERF_SEC_SAMPLE, SVAL_PERF_SEC_PTP_TASK, SVAL_PERF_SECTIONS };
void sval_perf_section(uint8_t section, uint32_t us);
void sval_perf_command(uint8_t *data, uint8_t length); // raw HID 0xFC op 0x43
bool sval_perf_split_rpc(uint8_t in_len, const uint8_t *in, uint8_t out_len, uint8_t *out);
