// Host test: the IQS5xx read schedule (keyboards/svalboard/azoteq/pad_schedule.h)
// against a simulated chip and main loop.
//
// The chip opens one communication window per report period. A read that
// starts before the window is held (clock stretching) until it opens; one
// that starts after it runs at once and, as the chip waits for it, delays
// that cycle. The pointing task only gets a chance once per 1 ms tick, a
// little after it (loop jitter), and never while a read is still running.
//
// First the old schedule (poll timer restarted at the start of each read, on
// the 1 ms timer) is run to show the model reproduces the measured lock-step:
// every read waiting ~6 ms. Then the new schedule must keep the chip's rate
// while waiting little, recover from timeouts without back-to-back reads,
// and survive the 32-bit microsecond clock wrapping.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "pad_schedule.h"

typedef struct {
    uint32_t period, xfer;  // chip report period, bare transfer (us)
    uint32_t window;        // next window opens (us)
    bool     restart_model; // the chip restarts its cycle when comms end
} chip_t;

typedef struct {
    unsigned reads, late, fails;
    uint64_t took_sum, busy;
    uint32_t took_max, first_start, last_end;
} stats_t;

static uint32_t rng = 12345;
static uint32_t jitter(uint32_t max) {
    rng = rng * 1103515245u + 12345u;
    return (rng >> 8) % (max + 1);
}

// One read starting at s. Returns its end; *ok false when fail is set (the
// bus is held until the 30 ms I2C timeout).
static uint32_t chip_read(chip_t *c, uint32_t s, bool fail, bool *late, uint32_t *took) {
    if (fail) {
        *late = false;
        *took = 30000;
        // The chip's cycles carry on under the failed transfer.
        while ((int32_t)(s + 30000 - c->window) > 0) c->window += c->period;
        return s + 30000;
    }
    uint32_t begin = s;
    *late          = (int32_t)(s - c->window) >= 0;
    if (!*late) begin = c->window; // stretched until the window opens
    uint32_t end = begin + c->xfer;
    *took        = end - s;
    if (c->restart_model)
        c->window = end + c->period;
    else
        c->window = (*late ? begin : c->window) + c->period; // a late read delays the cycle
    return end;
}

// The first pointing-task chance at or after t: a 1 ms tick plus loop jitter.
static uint32_t next_chance(uint32_t t, uint32_t base) {
    uint32_t rel  = t - base;
    uint32_t tick = (rel + 999) / 1000 * 1000;
    return base + tick + jitter(300);
}

static void add(stats_t *st, uint32_t s, uint32_t e, uint32_t took, bool late) {
    if (!st->reads) st->first_start = s;
    st->reads++;
    st->late += late;
    st->took_sum += took;
    st->busy += took;
    if (took > st->took_max) st->took_max = took;
    st->last_end = e;
}

// The old wrapper: timer_elapsed(azo_timer) >= interval on the ms timer,
// azo_timer = timer_read() at the start of the read.
static stats_t run_old(chip_t c, uint32_t base, unsigned n) {
    stats_t  st = {0};
    uint32_t t = base, last_ms = 0;
    bool     first = true;
    for (unsigned i = 0; i < n;) {
        t           = next_chance(t + 1, base);
        uint32_t ms = (t - base) / 1000;
        if (!first && ms - last_ms < c.period / 1000) continue;
        first   = false;
        last_ms = ms;
        bool     late;
        uint32_t took, e = chip_read(&c, t, false, &late, &took);
        if (i >= 50) add(&st, t, e, took, late);
        t = e;
        i++;
    }
    return st;
}

// The new wrapper. fail_from..fail_to: reads that time out.
static stats_t run_new(chip_t c, uint32_t base, unsigned n, uint32_t guess, unsigned skip, unsigned fail_from,
                       unsigned fail_to, pad_sched_t *out) {
    pad_sched_t s;
    pad_sched_init(&s, guess);
    stats_t  st = {0};
    uint32_t t = base, prev_fail_end = 0;
    bool     prev_failed = false;
    for (unsigned i = 0; i < n;) {
        t = next_chance(t + 1, base);
        if (!pad_sched_due(&s, t)) continue;
        bool     fail = i >= fail_from && i < fail_to, late;
        uint32_t took, e = chip_read(&c, t, fail, &late, &took);
        if (prev_failed) assert(t - prev_fail_end >= c.period); // never back to back
        pad_sched_done(&s, t, e, !fail, c.period);
        prev_failed   = fail;
        prev_fail_end = e;
        if (fail) st.fails++;
        if (i >= skip && !fail) add(&st, t, e, took, late);
        if (fail) st.busy += took;
        t = e;
        i++;
    }
    if (out) *out = s;
    return st;
}

static double avg_ms(const stats_t *st) {
    return st->took_sum / 1000.0 / st->reads;
}
static double rate_hz(const stats_t *st) {
    return (st->reads - 1) * 1e6 / (double)(st->last_end - st->first_start);
}

int main(void) {
    const uint32_t P = 8000, X = 1250, GUESS = 1070; // 400 kHz, 45-byte read
    chip_t         chip = {.period = P, .xfer = X, .window = 3700};

    // The model reproduces the measured lock-step with the old schedule.
    stats_t old = run_old(chip, 0, 2000);
    printf("old schedule:  %.1f Hz, read avg %.2f ms max %.2f ms, late %u\n", rate_hz(&old), avg_ms(&old),
           old.took_max / 1000.0, old.late);
    assert(avg_ms(&old) > 3.0); // every read waits (5.9 ms measured on the board)
    assert(rate_hz(&old) > 124 && rate_hz(&old) < 126);

    // New schedule, the chip's own rate kept, little waiting.
    pad_sched_t s;
    stats_t     st = run_new(chip, 0, 2000, GUESS, 50, ~0u, ~0u, &s);
    printf("new schedule:  %.1f Hz, read avg %.2f ms max %.2f ms, late %u of %u, lead %d us, xfer %u us\n",
           rate_hz(&st), avg_ms(&st), st.took_max / 1000.0, st.late, st.reads, (int)s.lead, s.xfer);
    assert(avg_ms(&st) < 2.2);
    assert(rate_hz(&st) > 122 && rate_hz(&st) < 126);
    assert(st.late < st.reads / 10);
    assert(s.xfer <= GUESS); // never above the guess

    // Every starting phase converges within a few reads.
    for (uint32_t w = 0; w < P; w += 500) {
        chip_t  c2 = chip;
        c2.window  = w;
        stats_t a  = run_new(c2, 0, 300, GUESS, 20, ~0u, ~0u, NULL);
        assert(avg_ms(&a) < 2.4);
        assert(rate_hz(&a) > 122);
    }

    // Timeouts: a full period after each failed read, backing off, and the
    // loop is never starved; afterwards the schedule settles again.
    stats_t f = run_new(chip, 0, 400, GUESS, 150, 100, 110, &s);
    printf("after timeouts: %.1f Hz, read avg %.2f ms, failures %u\n", rate_hz(&f), avg_ms(&f), f.fails);
    assert(f.fails == 10 && avg_ms(&f) < 2.2 && s.fails == 0);
    {
        pad_sched_t b;
        pad_sched_init(&b, GUESS);
        uint32_t t = 1000;
        uint32_t gaps[8];
        for (int i = 0; i < 8; i++) {
            pad_sched_done(&b, t, t + 30000, false, P);
            gaps[i] = b.due - (t + 30000);
            t       = b.due;
        }
        assert(gaps[0] == P && gaps[1] == 2 * P && gaps[2] == 4 * P && gaps[3] == 8 * P);
        assert(gaps[4] == 16 * P && gaps[7] == 16 * P); // bounded
        // Blocked at most 30 of every 30 + 128 ms once backed off.
        pad_sched_done(&b, t, t + 1300, true, P);
        assert(b.fails == 0 && b.due - (t + 1300) <= P);
    }

    // The microsecond clock wraps every 71 minutes.
    chip_t cw  = chip;
    uint32_t base = 0xFFFFFFFFu - 3000000u;
    cw.window     = base + 2000;
    stats_t w     = run_new(cw, base, 2000, GUESS, 50, ~0u, ~0u, NULL);
    assert(avg_ms(&w) < 2.2 && rate_hz(&w) > 122);

    // A chip that restarts its cycle when comms end: still short reads.
    chip_t  cr = chip;
    cr.restart_model = true;
    stats_t r  = run_new(cr, 0, 2000, GUESS, 50, ~0u, ~0u, NULL);
    stats_t ro = run_old(cr, 0, 2000);
    printf("restart model: new %.1f Hz avg %.2f ms, old %.1f Hz avg %.2f ms\n", rate_hz(&r), avg_ms(&r),
           rate_hz(&ro), avg_ms(&ro));
    assert(avg_ms(&r) < 2.2);

    // The release builds: 10 ms period, 10-byte read at 100 kHz.
    chip_t  rel = {.period = 10000, .xfer = 1750, .window = 9000};
    stats_t rs  = run_new(rel, 0, 1500, 1458, 50, ~0u, ~0u, NULL);
    stats_t rso = run_old(rel, 0, 1500);
    printf("release timing: new %.1f Hz avg %.2f ms, old %.1f Hz avg %.2f ms\n", rate_hz(&rs), avg_ms(&rs),
           rate_hz(&rso), avg_ms(&rso));
    assert(avg_ms(&rs) < 2.7 && rate_hz(&rs) > 97);

    // A bus slower than guessed (its bare read 400 us over the guess) still
    // keeps reads short and the rate within a few percent.
    chip_t  slow = chip;
    slow.xfer    = GUESS + 400;
    stats_t sl   = run_new(slow, 0, 2000, GUESS, 50, ~0u, ~0u, NULL);
    printf("slow bus:      %.1f Hz avg %.2f ms late %u\n", rate_hz(&sl), avg_ms(&sl), sl.late);
    assert(avg_ms(&sl) < 2.4 && rate_hz(&sl) > 118);

    puts("pad schedule: old lock-step reproduced; new keeps the chip rate with short reads from any phase, "
         "backs off on timeouts, survives the clock wrap, both chip models, release timing and a slow bus");
    return 0;
}
