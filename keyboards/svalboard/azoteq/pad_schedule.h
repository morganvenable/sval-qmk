// SPDX-License-Identifier: GPL-2.0-or-later
// When to read the IQS5xx next (azoteq.c). Pure logic, host-tested in
// tests/sval_native_ptp/test_schedule.c.
//
// The chip runs its own cycle at the report rate and opens one communication
// window per cycle. Addressing it before the window opens makes it stretch
// SCL until the window opens, and the main loop blocks for that whole time.
// Restarting the poll timer at the start of each read locks the poll to the
// chip's cycle at whatever phase it first had: measured at 8 ms, every read
// arrived about 6 ms early and blocked for 5.9 ms.
//
// So the next read is timed from the end of the previous one, one period
// later less a lead, and the lead adapts: each read's stretch (its time less
// the bare transfer time) is driven toward a small target. Late reads cost
// little (the window waits for us), early ones block, so the target sits a
// little early to absorb loop jitter and the pointing task's 1 ms tick.
//
// The bare transfer time is the shortest successful read seen, never above a
// conservative guess from the bus speed, so a read that waited is never taken
// for the bus being slow.
//
// A failed read carries no phase, so the next one waits a whole period after
// it ends, and consecutive failures back off further (bounded): a pad that
// holds the bus until the I2C timeout can never run reads back to back.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifndef SVAL_PAD_WAIT_TARGET_US
#    define SVAL_PAD_WAIT_TARGET_US 500 // aimed stretch per read
#endif
#ifndef SVAL_PAD_BACKOFF_MAX_SHIFT
#    define SVAL_PAD_BACKOFF_MAX_SHIFT 4 // failures: up to 16 periods apart
#endif

typedef struct {
    uint32_t due;   // next read at or after this time (us)
    uint32_t xfer;  // bare transfer time estimate (us)
    int32_t  lead;  // next read starts this long before end + period (us)
    uint32_t wait;  // last read's estimated stretch (us), for the counters
    uint8_t  fails; // consecutive failed reads
    bool     started;
} pad_sched_t;

static inline void pad_sched_init(pad_sched_t *s, uint32_t xfer_guess_us) {
    s->due     = 0;
    s->xfer    = xfer_guess_us;
    s->lead    = (int32_t)(xfer_guess_us + SVAL_PAD_WAIT_TARGET_US);
    s->wait    = 0;
    s->fails   = 0;
    s->started = false;
}

// Wrap-safe: true once now has reached due. The first read is always due.
static inline bool pad_sched_due(const pad_sched_t *s, uint32_t now_us) {
    return !s->started || (int32_t)(now_us - s->due) >= 0;
}

static inline void pad_sched_done(pad_sched_t *s, uint32_t start_us, uint32_t end_us, bool ok, uint32_t period_us) {
    // Only a read this schedule timed says anything about the lead: the first
    // read, and the first after a failure, land at an arbitrary phase.
    bool phased = s->started && s->fails == 0;
    s->started  = true;
    if (!ok) {
        uint8_t shift = s->fails < SVAL_PAD_BACKOFF_MAX_SHIFT ? s->fails : SVAL_PAD_BACKOFF_MAX_SHIFT;
        if (s->fails < 255) s->fails++;
        s->due = end_us + (period_us << shift);
        return;
    }
    s->fails      = 0;
    uint32_t took = end_us - start_us;
    if (took < s->xfer) s->xfer = took;
    s->wait = took - s->xfer;
    if (phased) {
        int32_t err = (int32_t)(s->wait > period_us ? period_us : s->wait) - SVAL_PAD_WAIT_TARGET_US;
        s->lead -= err / 4;
    }
    int32_t max = (int32_t)(period_us / 2);
    if (s->lead < 0) s->lead = 0;
    if (s->lead > max) s->lead = max;
    s->due = end_us + period_us - (uint32_t)s->lead;
}
