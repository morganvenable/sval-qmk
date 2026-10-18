// SPDX-License-Identifier: GPL-2.0-or-later
// When to read the IQS5xx next (azoteq.c, native touchpad builds). Pure
// logic, host-tested in tests/sval_native_ptp/test_schedule.c.
//
// The chip runs its own cycle at the report rate and opens one communication
// window per cycle. Addressing it before the window opens makes it stretch
// SCL until the window opens, and the main loop blocks for that whole time.
// Restarting the poll timer at the start of each read locks the poll to the
// chip's cycle at whatever phase it first had, so a read can block for most
// of a period.
//
// So the next read is timed from the end of the previous one, one period
// later less a lead, and the lead adapts: each read's stretch (its time less
// the bare transfer time) is driven toward a small target. Late reads cost
// little (the window waits for us), early ones block, so the target sits a
// little early to absorb loop jitter and the pointing task's 1 ms tick.
//
// Only the contact read itself is timed (azoteq_iqs5xx_last_read): not the
// end of session, nor the processing of the sample, nor a retry. Its length
// depends on the contact slots it carries (0 to 5), so the bare transfer time
// is kept per length: the shortest successful read seen of that length. The
// first read of a length learns it and leaves the lead alone.
//
// A failed read carries no phase, so the next one waits a whole period after
// it ends, and consecutive failures back off further, up to about a second: a
// pad that holds the bus until the I2C timeout costs at most a few percent of
// the main loop.
//
// Times are in microseconds on a free-running 32-bit clock. Every test is on
// elapsed time (now - end), so a long gap between calls never stops the reads.
#pragma once
#include <stdbool.h>
#include <stdint.h>

// Aimed average stretch per read. The main loop starts a due read up to a
// loop pass late (about 2 ms with an unpaced matrix scan), and a late read
// costs the chip no cycle, so a small target keeps most reads out of the
// chip's stretch: measured on the real-scan build, 50 us gave the same 123
// reads a second as 300 us with far fewer reads stretched past 1 ms.
#ifndef SVAL_PAD_WAIT_TARGET_US
#    define SVAL_PAD_WAIT_TARGET_US 50
#endif
// A read that waited less than the target landed after the window opened:
// the chip gave no phase, so the lead grows by this much. Together with the
// target it follows a chip clock 2% off ours (test_schedule.c) while few
// reads block past 1 ms.
#ifndef SVAL_PAD_LATE_STEP_US
#    define SVAL_PAD_LATE_STEP_US 25
#endif
// Read lengths, by contact slots carried with the base bytes.
#define SVAL_PAD_SIZES 6
#ifndef SVAL_PAD_BACKOFF_MAX_SHIFT
#    define SVAL_PAD_BACKOFF_MAX_SHIFT 7 // failures: up to 128 periods apart (1 s at 8 ms)
#endif

typedef struct {
    uint32_t end;   // the last read ended here (us)
    uint32_t delay; // the next read is due this long after end (us)
    uint32_t xfer[SVAL_PAD_SIZES]; // bare transfer time per length: the shortest successful read (us)
    int32_t  lead;  // the next read starts this long before end + period (us)
    uint32_t wait;  // last read's estimated stretch (us), for the counters
    uint8_t  fails; // consecutive failed reads
    bool     started;
} pad_sched_t;

static inline void pad_sched_init(pad_sched_t *s) {
    s->end     = 0;
    s->delay   = 0;
    for (uint8_t i = 0; i < SVAL_PAD_SIZES; i++)
        s->xfer[i] = UINT32_MAX;
    s->lead    = SVAL_PAD_WAIT_TARGET_US;
    s->wait    = 0;
    s->fails   = 0;
    s->started = false;
}

// True once a read is due. The first read is always due.
static inline bool pad_sched_due(const pad_sched_t *s, uint32_t now_us) {
    return !s->started || now_us - s->end >= s->delay;
}

// After a read: start_us and end_us bracket the contact read itself; phased
// is false for a read whose timing says nothing about the chip's window (a
// retry after a NACK); size is the contact slots the read carried.
static inline void pad_sched_done(pad_sched_t *s, uint32_t start_us, uint32_t end_us, bool ok, bool phased, uint8_t size, uint32_t period_us) {
    if (size >= SVAL_PAD_SIZES) size = SVAL_PAD_SIZES - 1;
    // Only a read this schedule timed says anything about the lead: the first
    // read, and the first after a failure, land at an arbitrary phase.
    phased = phased && s->started && s->fails == 0;
    if (!s->started) s->lead = (int32_t)(period_us / 4); // start early: the first reads find the phase
    s->started = true;
    s->end     = end_us;
    if (!ok) {
        uint8_t shift = s->fails < SVAL_PAD_BACKOFF_MAX_SHIFT ? s->fails : SVAL_PAD_BACKOFF_MAX_SHIFT;
        if (s->fails < 255) s->fails++;
        s->delay = period_us << shift;
        return;
    }
    s->fails      = 0;
    uint32_t took  = end_us - start_us;
    bool     known = s->xfer[size] != UINT32_MAX;
    if (took < s->xfer[size]) s->xfer[size] = took;
    s->wait = took - s->xfer[size];
    if (phased && known) {
        if (s->wait < SVAL_PAD_WAIT_TARGET_US)
            s->lead += SVAL_PAD_LATE_STEP_US; // late: aim earlier
        else
            s->lead -= ((int32_t)(s->wait > period_us ? period_us : s->wait) - SVAL_PAD_WAIT_TARGET_US) / 4;
    }
    int32_t max = (int32_t)(period_us / 2);
    if (s->lead < 0) s->lead = 0;
    if (s->lead > max) s->lead = max;
    s->delay = period_us - (uint32_t)s->lead;
}
