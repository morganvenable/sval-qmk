// Host test: the IQS5xx read schedule (keyboards/svalboard/azoteq/pad_schedule.h)
// against a simulated chip and main loop.
//
// The chip opens one communication window per report period. A read that
// starts before the window is held (clock stretching) until it opens; one
// that starts after it runs at once. Measured on the board (real-scan build),
// a late read does not delay the chip's cycle: 123 reads a second whether
// half the reads were late or almost all were. The restart model (the cycle
// starts again when comms end) is kept as the pessimistic case. The pointing task only gets a chance once per 1 ms tick, a
// little after it (loop jitter), and never while a read is still running.
//
// First the old schedule (poll timer restarted at the start of each read, on
// the 1 ms timer) is run to show the model reproduces the measured lock-step:
// every read waiting ~6 ms. Then the new schedule must keep the chip's rate
// while waiting little, recover from timeouts without back-to-back reads,
// back off a dead pad to about a second, survive the 32-bit microsecond clock
// wrapping and a long gap between calls, and keep the rate with the main loop
// measured on the real-scan build (a pass every ~2.1 ms).
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "pad_schedule.h"

typedef struct {
    uint32_t period, xfer;  // chip report period, bare transfer (us)
    uint32_t window;        // next window opens (us)
    bool     restart_model; // the chip restarts its cycle when comms end
    uint32_t nominal;       // the period the firmware assumes (0: the chip's)
    uint32_t per_slot;      // transfer time each contact slot adds (us)
} chip_t;

// Contact slots carried by each read: 0 throughout, or with vary_slots a
// mix of 0 to 5 that changes every few reads (fingers landing and lifting).
static bool    vary_slots = false;
static uint8_t slots_for(unsigned i) {
    static const uint8_t pattern[] = {0, 1, 1, 2, 2, 3, 5, 4, 1, 0, 3, 2};
    return vary_slots ? pattern[(i / 7) % sizeof(pattern)] : 0;
}

typedef struct {
    unsigned reads, late, fails, long_waits; // long_waits: stretched over 1 ms
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
static uint32_t chip_read(chip_t *c, uint32_t s, uint8_t slots, bool fail, bool *late, uint32_t *took) {
    if (fail) {
        *late = false;
        *took = 30000;
        // The chip's cycles carry on under the failed transfer.
        while ((int32_t)(s + 30000 - c->window) > 0)
            c->window += c->period;
        return s + 30000;
    }
    uint32_t begin = s;
    *late          = (int32_t)(s - c->window) >= 0;
    if (!*late) begin = c->window; // stretched until the window opens
    uint32_t end = begin + c->xfer + slots * c->per_slot;
    *took        = end - s;
    if (c->restart_model)
        c->window = end + c->period;
    else
        while ((int32_t)(end - c->window) >= 0)
            c->window += c->period; // the chip keeps its own cycle
    return end;
}

// The first pointing-task chance at or after t. By default the pointing
// task's 1 ms tick plus loop jitter. With free_loop, main-loop passes that
// follow one another (the real-scan build: an unpaced matrix scan makes each
// pass about 2 ms): the next chance is a pass length after t.
static uint32_t loop_us = 1000, loop_jitter = 300;
static bool     free_loop = false;
static uint32_t next_chance(uint32_t t, uint32_t base) {
    if (free_loop) return t + loop_us - loop_jitter / 2 + jitter(loop_jitter);
    uint32_t rel  = t - base;
    uint32_t tick = (rel + loop_us - 1) / loop_us * loop_us;
    return base + tick + jitter(loop_jitter);
}

static void add(stats_t *st, uint32_t s, uint32_t e, uint32_t took, bool late, uint32_t xfer) {
    if (!st->reads) st->first_start = s;
    st->reads++;
    st->late += late;
    st->long_waits += took > xfer + 1000;
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
        uint32_t took, e = chip_read(&c, t, 0, false, &late, &took);
        if (i >= 50) add(&st, t, e, took, late, c.xfer);
        t = e;
        i++;
    }
    return st;
}

// The new wrapper. fail_from..fail_to: reads that time out.
static stats_t run_new(chip_t c, uint32_t base, unsigned n, unsigned skip, unsigned fail_from, unsigned fail_to, pad_sched_t *out) {
    pad_sched_t s;
    pad_sched_init(&s);
    stats_t  st = {0};
    uint32_t t = base, prev_fail_end = 0;
    bool     prev_failed = false;
    for (unsigned i = 0; i < n;) {
        t = next_chance(t + 1, base);
        if (!pad_sched_due(&s, t)) continue;
        bool     fail  = i >= fail_from && i < fail_to, late;
        uint8_t  slots = slots_for(i);
        uint32_t took, e = chip_read(&c, t, slots, fail, &late, &took);
        if (prev_failed) assert(t - prev_fail_end >= c.period); // never back to back
        pad_sched_done(&s, t, e, !fail, true, slots, c.nominal ? c.nominal : c.period);
        prev_failed   = fail;
        prev_fail_end = e;
        if (fail) st.fails++;
        if (i >= skip && !fail) add(&st, t, e, took, late, c.xfer + slots * c.per_slot);
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
    setvbuf(stdout, NULL, _IONBF, 0);
    const uint32_t P = 8000, X = 1250; // 400 kHz, 45-byte read
    chip_t         chip = {.period = P, .xfer = X, .window = 3700};

    // The model reproduces the measured lock-step with the old schedule.
    stats_t old = run_old(chip, 0, 2000);
    printf("old schedule:  %.1f Hz, read avg %.2f ms max %.2f ms, late %u\n", rate_hz(&old), avg_ms(&old), old.took_max / 1000.0, old.late);
    assert(avg_ms(&old) > 3.0); // every read waits (5.9 ms measured on the board)
    assert(rate_hz(&old) > 124 && rate_hz(&old) < 126);

    // New schedule, the chip's own rate kept, little waiting.
    pad_sched_t s;
    stats_t     st = run_new(chip, 0, 2000, 50, ~0u, ~0u, &s);
    printf("new schedule:  %.1f Hz, read avg %.2f ms max %.2f ms, late %u of %u, lead %d us, xfer %u us\n", rate_hz(&st), avg_ms(&st), st.took_max / 1000.0, st.late, st.reads, (int)s.lead, s.xfer[0]);
    assert(st.late > st.reads / 2); // most reads land just after the window opens
    assert(avg_ms(&st) < 2.2);
    assert(rate_hz(&st) > 122 && rate_hz(&st) < 126);
    assert(st.long_waits < st.reads / 50);          // few reads blocked past 1 ms
    assert(s.xfer[0] >= X && s.xfer[0] <= X + 300); // the shortest read seen: about the bare transfer

    // Every starting phase converges within a few reads.
    for (uint32_t w = 0; w < P; w += 500) {
        chip_t c2 = chip;
        c2.window = w;
        stats_t a = run_new(c2, 0, 300, 20, ~0u, ~0u, NULL);
        assert(avg_ms(&a) < 2.4);
        assert(rate_hz(&a) > 122);
    }

    // Timeouts: a full period after each failed read, backing off, and the
    // loop is never starved; afterwards the schedule settles again.
    stats_t f = run_new(chip, 0, 400, 150, 100, 110, &s);
    printf("after timeouts: %.1f Hz, read avg %.2f ms, failures %u\n", rate_hz(&f), avg_ms(&f), f.fails);
    assert(f.fails == 10 && avg_ms(&f) < 2.2 && s.fails == 0);
    {
        // A dead pad that holds the bus until the 30 ms timeout: the gaps
        // double up to 128 periods (about 1 s), so it costs at most 30 ms of
        // every second of the main loop once backed off.
        pad_sched_t b;
        pad_sched_init(&b);
        uint32_t t = 1000;
        uint32_t gaps[10];
        for (int i = 0; i < 10; i++) {
            pad_sched_done(&b, t, t + 30000, false, true, 0, P);
            gaps[i] = b.delay;
            assert(!pad_sched_due(&b, t + 30000 + b.delay - 1) && pad_sched_due(&b, t + 30000 + b.delay));
            t = t + 30000 + b.delay;
        }
        assert(gaps[0] == P && gaps[1] == 2 * P && gaps[2] == 4 * P && gaps[3] == 8 * P);
        assert(gaps[7] == 128 * P && gaps[9] == 128 * P); // bounded
        double blocked = 30000.0 / (30000.0 + gaps[9]);
        printf("dead pad:      blocked %.1f%% of the loop once backed off\n", 100 * blocked);
        assert(blocked < 0.03);
        // It answers again: back to one period at once.
        pad_sched_done(&b, t, t + 1300, true, true, 0, P);
        assert(b.fails == 0 && b.delay <= P);
        // A retried read (after a NACK) says nothing about the window.
        int32_t lead = b.lead;
        pad_sched_done(&b, t + 9000, t + 9000 + 5000, true, false, 0, P);
        assert(b.lead == lead);
    }

    // A long gap between calls (the loop held up, or the 32-bit clock far
    // past the last read): the next read is due at once, never stuck.
    {
        pad_sched_t g;
        pad_sched_init(&g);
        pad_sched_done(&g, 1000, 2300, true, true, 0, P);
        assert(pad_sched_due(&g, 2300 + 40u * 60 * 1000000)); // 40 minutes later
        assert(pad_sched_due(&g, 2300 + 0x80000000u));        // half the clock later
        assert(!pad_sched_due(&g, 2300 + 1000));
    }

    // The microsecond clock wraps every 71 minutes.
    chip_t   cw   = chip;
    uint32_t base = 0xFFFFFFFFu - 3000000u;
    cw.window     = base + 2000;
    stats_t w     = run_new(cw, base, 2000, 50, ~0u, ~0u, NULL);
    assert(avg_ms(&w) < 2.2 && rate_hz(&w) > 122);

    // A chip that restarts its cycle when comms end: still short reads.
    chip_t cr        = chip;
    cr.restart_model = true;
    stats_t r        = run_new(cr, 0, 2000, 50, ~0u, ~0u, NULL);
    stats_t ro       = run_old(cr, 0, 2000);
    printf("restart model: new %.1f Hz avg %.2f ms, old %.1f Hz avg %.2f ms\n", rate_hz(&r), avg_ms(&r), rate_hz(&ro), avg_ms(&ro));
    assert(avg_ms(&r) < 2.2);

    // Rate under the restart model: the chip's next window opens a period
    // after the session ends, so a read every period + transfer is the most
    // it can give. A late read costs rate there; the small target still keeps
    // most of it (the board is not this model: see the top).
    double restart_max = 1e6 / (P + X);
    printf("restart model: %.1f Hz of at most %.1f Hz\n", rate_hz(&r), restart_max);
    assert(rate_hz(&r) > restart_max * 0.93);

    // The main loop measured on the real-scan build (unpaced scan, turbo 0):
    // a pass every ~2.1 ms (1.4 to 2.8), so a read starts up to a pass after it is due.
    // The board measured 123 reads a second here, with few reads blocked.
    loop_us     = 2100;
    loop_jitter = 1400;
    free_loop   = true;
    stats_t ls  = run_new(chip, 0, 2000, 50, ~0u, ~0u, &s);
    printf("2.1 ms loop:   %.1f Hz, read avg %.2f ms max %.2f ms, blocked over 1 ms %u of %u, lead %d us\n", rate_hz(&ls), avg_ms(&ls), ls.took_max / 1000.0, ls.long_waits, ls.reads, (int)s.lead);
    assert(rate_hz(&ls) > 122 && rate_hz(&ls) < 126);
    assert(ls.long_waits < ls.reads / 20);
    assert(avg_ms(&ls) < (X + 2 * SVAL_PAD_WAIT_TARGET_US) / 1000.0);
    // The chip's clock is not ours: 2% slow or fast, the schedule follows it
    // (every chip cycle read once, few reads blocked).
    for (int d = -1; d <= 1; d += 2) {
        chip_t cd  = chip;
        cd.period  = (uint32_t)((int32_t)P + d * (int32_t)P / 50);
        cd.nominal = P;
        stats_t ds = run_new(cd, 0, 3000, 100, ~0u, ~0u, NULL);
        double  hz = 1e6 / cd.period;
        printf("chip clock %+d%%: %.1f Hz of %.1f, blocked over 1 ms %u of %u\n", 2 * d, rate_hz(&ds), hz, ds.long_waits, ds.reads);
        assert(rate_hz(&ds) > hz * 0.99 && rate_hz(&ds) < hz * 1.01);
        assert(ds.long_waits < ds.reads / 20);
    }
    loop_us     = 1000;
    loop_jitter = 300;
    free_loop   = false;

    // A first read of 10 bytes (no finger down): the same schedule.
    chip_t base10 = chip;
    base10.xfer   = 350;
    stats_t b10   = run_new(base10, 0, 2000, 50, ~0u, ~0u, NULL);
    printf("10-byte reads: %.1f Hz avg %.2f ms\n", rate_hz(&b10), avg_ms(&b10));
    assert(rate_hz(&b10) > 122 && avg_ms(&b10) < 1.3);

    // Reads sized to the contacts down: 10 bytes plus 7 a slot (about 160 us
    // a slot at 400 kHz), the slot count changing as fingers land and lift.
    // A longer read is not mistaken for a stretch: the rate and the short
    // waits hold, and each length learns its own bare transfer.
    {
        chip_t sized   = base10;
        sized.per_slot = 160;
        vary_slots     = true;
        pad_sched_t vs;
        stats_t     v = run_new(sized, 0, 3000, 100, ~0u, ~0u, &vs);
        vary_slots    = false;
        printf("sized reads:   %.1f Hz read avg %.2f ms, blocked over 1 ms %u of %u, lead %d us\n", rate_hz(&v), avg_ms(&v), v.long_waits, v.reads, (int)vs.lead);
        assert(rate_hz(&v) > 122 && rate_hz(&v) < 126);
        assert(v.long_waits < v.reads / 50);
        for (uint8_t k = 0; k < SVAL_PAD_SIZES; k++)
            assert(vs.xfer[k] >= base10.xfer + k * sized.per_slot && vs.xfer[k] <= base10.xfer + k * sized.per_slot + 300);
    }

    puts("pad schedule: old lock-step reproduced; new keeps the chip rate with short reads from any phase, "
         "backs off a dead pad to ~1 s, survives the clock wrap and long gaps, reaches the restart-model rate, keeps the rate with a 2.1 ms loop, 10-byte reads and reads sized to the contacts");
    return 0;
}
