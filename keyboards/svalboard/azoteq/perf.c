// SPDX-License-Identifier: GPL-2.0-or-later
// Opt-in timing counters for the touchpad experiments (SVAL_PERF_STATS=yes).
// Kept on each half; read over raw HID 0xFC op 0x43 (the other half through
// the KEYBOARD_MT RPC). Host side: tools/multitouch/perf.py. Times are in
// microseconds from the 1 MHz system timer.
#include "quantum.h"
#include "perf.h"
#include <string.h>
#ifdef SVAL_NATIVE_PTP
#    include "ptp.h"
#endif

typedef struct {
    uint32_t since_ms, last_loop, last_scan;
    uint32_t loops, loop_sum, loop_max, loop_hist[SVAL_PERF_BUCKETS];
    uint32_t scans, scan_sum, scan_max, scan_hist[SVAL_PERF_BUCKETS];
    uint32_t i2c_n, i2c_sum, i2c_max, i2c_fail, i2c_hist[SVAL_PERF_BUCKETS];
    uint32_t rpc_n, rpc_fail, rpc_sum, rpc_max, rpc_new;
} perf_t;

static perf_t s;

uint32_t sval_perf_now_us(void) {
    return (uint32_t)chVTGetSystemTimeX();
}

// <1, 1-2, 2-4, 4-8, 8-16, >=16 ms
static uint8_t bucket(uint32_t us) {
    uint8_t b = 0;
    for (uint32_t edge = 1000; b < SVAL_PERF_BUCKETS - 1 && us >= edge; edge <<= 1) b++;
    return b;
}

static void add(uint32_t us, uint32_t *n, uint32_t *sum, uint32_t *max, uint32_t *hist) {
    (*n)++;
    *sum += us;
    if (us > *max) *max = us;
    if (hist) hist[bucket(us)]++;
}

void sval_perf_reset(void) {
    memset(&s, 0, sizeof(s));
    s.since_ms = timer_read32();
}

void sval_perf_loop(void) {
    uint32_t now = sval_perf_now_us();
    if (s.last_loop) add(now - s.last_loop, &s.loops, &s.loop_sum, &s.loop_max, s.loop_hist);
    s.last_loop = now;
}

void sval_perf_scan(uint32_t now) {
    if (s.last_scan) add(now - s.last_scan, &s.scans, &s.scan_sum, &s.scan_max, s.scan_hist);
    s.last_scan = now;
}

void sval_perf_i2c(uint32_t us, bool ok) {
    add(us, &s.i2c_n, &s.i2c_sum, &s.i2c_max, s.i2c_hist);
    if (!ok) s.i2c_fail++;
}

void sval_perf_rpc(uint32_t us, bool ok, bool fresh) {
    add(us, &s.rpc_n, &s.rpc_sum, &s.rpc_max, NULL);
    if (!ok) s.rpc_fail++;
    if (fresh) s.rpc_new++;
}

// Three pages of 13 counters; layout in tools/multitouch/perf.py.
bool sval_perf_page(uint8_t page, uint32_t out[SVAL_PERF_PAGE_WORDS]) {
    memset(out, 0, SVAL_PERF_PAGE_WORDS * sizeof(uint32_t));
    uint32_t frames = 0, busy = 0;
#ifdef SVAL_NATIVE_PTP
    uint8_t st[32] = {0};
    sval_ptp_status(st);
    frames = st[6] | (uint32_t)st[7] << 8 | (uint32_t)st[8] << 16 | (uint32_t)st[9] << 24;
    busy   = st[10] | (uint32_t)st[11] << 8 | (uint32_t)st[12] << 16 | (uint32_t)st[13] << 24;
#endif
    switch (page) {
        case 0: {
            const uint32_t v[] = {timer_elapsed32(s.since_ms), s.loops, s.loop_sum, s.loop_max, s.scans, s.scan_sum, s.scan_max, s.i2c_n, s.i2c_sum, s.i2c_max, s.i2c_fail, s.rpc_n, s.rpc_fail};
            memcpy(out, v, sizeof(v));
            return true;
        }
        case 1: {
            const uint32_t v[] = {s.rpc_sum, s.rpc_max, s.rpc_new, frames, busy};
            memcpy(out, v, sizeof(v));
            memcpy(out + 5, s.loop_hist, sizeof(s.loop_hist));
            return true;
        }
        case 2:
            memcpy(out, s.i2c_hist, sizeof(s.i2c_hist));
            memcpy(out + SVAL_PERF_BUCKETS, s.scan_hist, sizeof(s.scan_hist));
            return true;
    }
    return false;
}
