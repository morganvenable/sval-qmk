// Host tests for kb/debounce_sval.c and kb/chatter.c. Run by util/updater_test/run.sh.
#include "debounce_sval_host.h"
#include "chatter.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef SVAL_DD_DEBOUNCE_MS
#    error "build with -DSVAL_DD_DEBOUNCE_MS=N"
#endif
#define DD_MS SVAL_DD_DEBOUNCE_MS
_Static_assert(DD_MS >= 3 && DD_MS <= 40, "the timing patterns below assume 3..40 ms");
#define DD ((matrix_row_t)1 << DOUBLEDOWN_COL)

static uint16_t     now_ms;
static matrix_row_t raw[MATRIX_ROWS_PER_HAND], cooked[MATRIX_ROWS_PER_HAND];
static int          failures;

uint16_t host_now_ms(void) {
    return now_ms;
}

#define CHECK(cond, ...)                                \
    do {                                                \
        if (!(cond)) {                                  \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                        \
            printf("\n");                               \
            failures++;                                 \
        }                                               \
    } while (0)

static void reset(uint16_t t) {
    now_ms = t;
    memset(raw, 0, sizeof(raw));
    memset(cooked, 0, sizeof(cooked));
    debounce_init();
}

// One pass: debounce() runs on every matrix_scan(), changed or not.
static bool scan(void) {
    return debounce(raw, cooked, true);
}

// Scan once per ms until the DD bit in cooked equals want; the ms taken, or -1.
static int run_until_dd(bool want, int max_ms) {
    for (int i = 0; i <= max_ms; i++) {
        scan();
        if (((cooked[0] & DD) != 0) == want) return i;
        now_ms++;
    }
    return -1;
}

static void test_optical_pass_through(void) {
    reset(100);
    // Every optical key, press and release, reaches cooked on the same scan.
    for (int r = 0; r < MATRIX_ROWS_PER_HAND; r++) {
        for (int c = 0; c < 6; c++) {
            if (r == 0 && c == DOUBLEDOWN_COL) continue;
            matrix_row_t bit = (matrix_row_t)1 << c;
            raw[r] |= bit;
            CHECK(scan(), "press r%d c%d reported", r, c);
            CHECK(cooked[r] & bit, "press r%d c%d immediate", r, c);
            raw[r] &= (matrix_row_t)~bit;
            CHECK(scan(), "release r%d c%d reported", r, c);
            CHECK(!(cooked[r] & bit), "release r%d c%d immediate", r, c);
        }
    }
    // A chord: all at once, on one scan.
    raw[1] = 0x3F;
    raw[4] = 0x15;
    CHECK(scan() && cooked[1] == 0x3F && cooked[4] == 0x15, "chord immediate");
    raw[1] = raw[4] = 0;
    CHECK(scan() && cooked[1] == 0 && cooked[4] == 0, "chord release immediate");
    // A one-scan glitch on an optical key is not filtered: the chatter log watches for it.
    raw[2] = 0x01;
    scan();
    CHECK(cooked[2] == 0x01, "optical glitch passes");
    raw[2] = 0;
    scan();
    CHECK(cooked[2] == 0, "glitch gone");
    CHECK(!scan(), "idle scan reports no change");
}

static void test_finger_col6_not_debounced(void) {
    reset(7);
    // COL6 on the finger rows is Double South: optical, no delay.
    for (int r = 1; r < MATRIX_ROWS_PER_HAND; r++) {
        raw[r] = DD;
        scan();
        CHECK(cooked[r] == DD, "row %d COL6 press immediate", r);
        raw[r] = 0;
        scan();
        CHECK(cooked[r] == 0, "row %d COL6 release immediate", r);
    }
}

static void test_dd_clean(void) {
    reset(1000);
    raw[0] = DD;
    int t  = run_until_dd(true, 50);
    CHECK(t == DD_MS, "clean DD press after %d ms (want %d)", t, DD_MS);
    raw[0] = 0;
    t      = run_until_dd(false, 50);
    CHECK(t == DD_MS, "clean DD release after %d ms (want %d)", t, DD_MS);
}

static int feed_dd(const char *pattern, bool *last, int *rise_at) {
    int changes = 0;
    for (int i = 0; pattern[i]; i++) {
        raw[0] = pattern[i] == '1' ? DD : 0;
        scan();
        bool now = (cooked[0] & DD) != 0;
        if (now != *last) {
            changes++;
            if (rise_at) *rise_at = i;
        }
        *last = now;
        now_ms++;
    }
    return changes;
}

static void test_dd_bounce(void) {
    reset(2000);
    bool last = false;
    int  at   = -1;
    // Press with contact bounce, one sample per ms: cooked changes once,
    // DD_MS after the last bounce (index 4).
    int changes = feed_dd("101011111111111111", &last, &at);
    CHECK(changes == 1 && last, "bouncy press: %d changes", changes);
    CHECK(at == 4 + DD_MS, "bouncy press settled at %d ms (want %d)", at, 4 + DD_MS);
    // Release with bounce.
    changes = feed_dd("010100000000000000", &last, NULL);
    CHECK(changes == 1 && !last, "bouncy release: %d changes, final %d", changes, last);
}

static void test_dd_glitch_filtered(void) {
    reset(3000);
    bool last = false;
    // One-frame glitches (the polarity check at boot, or noise) never report.
    CHECK(feed_dd("1000100010001000100000", &last, NULL) == 0, "single glitches filtered");
    // Pulses one ms shorter than DD_MS, repeating: still nothing.
    char p[64] = {0};
    int  n     = 0;
    for (int k = 0; k < 4; k++) {
        for (int i = 0; i < DD_MS - 1; i++)
            p[n++] = '1';
        p[n++] = '0';
    }
    CHECK(feed_dd(p, &last, NULL) == 0, "short pulses filtered");
}

static void test_dd_independent_of_optical(void) {
    reset(4000);
    // While the DD settles, other keys pass at once and do not restart its
    // timer (per key, not global as sym_defer_g was).
    raw[0] = DD;
    scan();
    now_ms++;
    raw[0] = DD | 0x04; // thumb Down
    scan();
    CHECK(cooked[0] == 0x04, "thumb Down immediate while DD settles (0x%02x)", cooked[0]);
    now_ms++;
    raw[0] = DD;
    raw[3] = 0x02;
    scan();
    CHECK(cooked[0] == 0 && cooked[3] == 0x02, "others immediate");
    int t = run_until_dd(true, 50);
    CHECK(t == DD_MS - 2, "DD after %d more ms (want %d)", t, DD_MS - 2);
    CHECK(cooked[3] == 0x02, "finger key kept");
}

static void test_timer_wrap(void) {
    reset(65533);
    raw[0] = DD;
    int t  = run_until_dd(true, 50);
    CHECK(t == DD_MS, "press across the 16-bit wrap after %d ms", t);
}

static void test_chatter(void) {
    uint8_t a[5] = {0}, b[5] = {0};
    sval_chatter_clear(0);
    const sval_chatter_t *s = sval_chatter_stats();
    // Human-speed taps on r1c0: 40 ms down, 60 ms up. No fast transitions.
    uint32_t t = 10000;
    for (int k = 0; k < 10; k++) {
        b[1] = 0x01;
        sval_chatter_frame(a, b, t);
        memcpy(a, b, 5);
        t += 40000;
        b[1] = 0;
        sval_chatter_frame(a, b, t);
        memcpy(a, b, 5);
        t += 60000;
    }
    CHECK(s->transitions[1][0] == 20, "20 transitions (%u)", s->transitions[1][0]);
    CHECK(s->fast[1][0] == 0, "no fast transitions");
    CHECK(s->min_gap_us[1][0] == 40000, "min gap 40 ms (%u)", s->min_gap_us[1][0]);
    // Chatter on r0c5: down, up 1 ms later, down again 1.5 ms after that.
    const uint32_t at[3] = {t, t + 1000, t + 2500};
    for (int k = 0; k < 3; k++) {
        b[0] = (k & 1) ? 0 : 0x20;
        sval_chatter_frame(a, b, at[k]);
        memcpy(a, b, 5);
    }
    CHECK(s->transitions[0][5] == 3, "3 transitions");
    CHECK(s->fast[0][5] == 2, "2 fast (%u)", s->fast[0][5]);
    CHECK(s->min_gap_us[0][5] == 1000, "min gap 1000 us (%u)", s->min_gap_us[0][5]);
    CHECK(s->fast[1][0] == 0 && s->transitions[2][0] == 0, "other keys untouched");
    // A gap of exactly the limit is not fast.
    b[0] = 0;
    sval_chatter_frame(a, b, at[2] + SVAL_CHATTER_FAST_US);
    CHECK(s->fast[0][5] == 2, "gap at the limit is not fast");
    sval_chatter_clear(t);
    CHECK(s->transitions[0][5] == 0 && s->min_gap_us[0][5] == 0xFFFF && s->seen[0] == 0, "clear");
}

int main(void) {
    test_optical_pass_through();
    test_finger_col6_not_debounced();
    test_dd_clean();
    test_dd_bounce();
    test_dd_glitch_filtered();
    test_dd_independent_of_optical();
    test_timer_wrap();
    test_chatter();
    if (failures) {
        printf("debounce tests: %d failures\n", failures);
        return 1;
    }
    printf("debounce tests: ok\n");
    return 0;
}
