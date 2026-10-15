// SPDX-License-Identifier: GPL-2.0-or-later
// Timing counters (SVAL_PERF_STATS, dev builds only); see perf.c.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define SVAL_PERF_BUCKETS 6
#define SVAL_PERF_PAGE_WORDS 13
#define SVAL_PERF_PAGES 6
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
// What the main loop was doing in the slowest scan gap (page 5). Each part's
// time is exclusive: a USB send inside a split fetch counts as USB only.
// sval_perf_enter() makes part current and returns the one it interrupted,
// for sval_perf_leave(). Main loop only.
enum {
    SVAL_PERF_GAP_OTHER,    // everything not below: key processing, housekeeping, the core
    SVAL_PERF_GAP_MATRIX,   // the matrix row reads (matrix.c)
    SVAL_PERF_GAP_SYNC,     // the split exchange of every scan (matrix_post_scan, split_pause.c)
    SVAL_PERF_GAP_PAD,      // the pad driver call (azoteq/azoteq.c)
    SVAL_PERF_GAP_FETCH,    // the other pad over the split link (azoteq/multitouch.c)
    SVAL_PERF_GAP_USB,      // USB IN sends (tmk_core/protocol/chibios/usb_driver.c)
    SVAL_PERF_GAP_COUNTERS, // these counters' own reads of the other half
    SVAL_PERF_GAP_PARTS
};
uint8_t sval_perf_enter(uint8_t part);
void    sval_perf_leave(uint8_t prev);
void sval_perf_command(uint8_t *data, uint8_t length); // raw HID 0xFC op 0x43
bool sval_perf_split_rpc(uint8_t in_len, const uint8_t *in, uint8_t out_len, uint8_t *out);
