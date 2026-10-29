// Host tests for keyboards/svalboard/debounce_sval.c. Run by util/updater_test/run.sh.
//
// The firmware calls debounce() on every matrix_scan() pass; these tests call
// it once per simulated millisecond.
#include "debounce_sval_host.h"
#include <stdio.h>
#include <string.h>

#ifndef SVAL_DD_DEBOUNCE_MS
#    define SVAL_DD_DEBOUNCE_MS 5
#endif
#define DD_MS SVAL_DD_DEBOUNCE_MS
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

static bool scan(void) {
    return debounce(raw, cooked, true);
}

static bool dd_cooked(void) {
    return (cooked[0] & DD) != 0;
}

// Scan once per ms until the debounced Double Down equals want: the ms taken, or -1.
static int run_until_dd(bool want, int max_ms) {
    for (int i = 0; i <= max_ms; i++) {
        scan();
        if (dd_cooked() == want) return i;
        now_ms++;
    }
    return -1;
}

// Every optical key, finger rows' COL6 (Double South) included, changes on
// the same scan its raw bit flips, press and release, and so does a chord.
static void test_optical_same_scan(void) {
    reset(100);
    for (int r = 0; r < MATRIX_ROWS_PER_HAND; r++) {
        for (int c = 0; c < 6; c++) {
            if (r == 0 && c == DOUBLEDOWN_COL) continue;
            matrix_row_t bit = (matrix_row_t)1 << c;
            raw[r] |= bit;
            CHECK(scan(), "r%d c%d press reported", r, c);
            CHECK(cooked[r] & bit, "r%d c%d press on the same scan", r, c);
            now_ms++;
            raw[r] &= (matrix_row_t)~bit;
            CHECK(scan(), "r%d c%d release reported", r, c);
            CHECK(!(cooked[r] & bit), "r%d c%d release on the same scan", r, c);
            now_ms++;
        }
    }
    raw[1] = 0x3F;
    raw[4] = 0x15;
    CHECK(scan() && cooked[1] == 0x3F && cooked[4] == 0x15, "chord on the same scan");
    raw[1] = raw[4] = 0;
    CHECK(scan() && cooked[1] == 0 && cooked[4] == 0, "chord release on the same scan");
    CHECK(!scan(), "an unchanged scan reports nothing");
    // An optical key flipping every scan follows raw exactly: nothing is filtered.
    for (int i = 0; i < 10; i++) {
        raw[2] ^= 0x01;
        scan();
        CHECK(cooked[2] == raw[2], "optical flip %d followed", i);
        now_ms++;
    }
}

// A clean Double Down press and release each report after DD_MS.
static void test_dd_clean(void) {
    reset(1000);
    raw[0] = DD;
    int t  = run_until_dd(true, 50);
    CHECK(t == DD_MS, "clean press after %d ms (want %d)", t, DD_MS);
    raw[0] = 0;
    t      = run_until_dd(false, 50);
    CHECK(t == DD_MS, "clean release after %d ms (want %d)", t, DD_MS);
}

// Feed one raw Double Down level per ms; count debounced changes.
static int feed_dd(const char *pattern, int *last_change_at) {
    int  changes = 0;
    bool last    = dd_cooked();
    for (int i = 0; pattern[i]; i++) {
        raw[0] = (raw[0] & (matrix_row_t)~DD) | (pattern[i] == '1' ? DD : 0);
        scan();
        if (dd_cooked() != last) {
            changes++;
            if (last_change_at) *last_change_at = i;
            last = dd_cooked();
        }
        now_ms++;
    }
    return changes;
}

// A bouncing press settles DD_MS after the last bounce, as one press; a
// bouncing release, as one release; glitches shorter than DD_MS never report.
static void test_dd_bounce(void) {
    reset(2000);
    int at = -1;
    //                      bounces for 6 ms, then held
    int n  = feed_dd("101101" "1111111111", &at);
    CHECK(n == 1, "bouncing press: %d changes (want 1)", n);
    CHECK(dd_cooked(), "bouncing press settles down");
    CHECK(at == 5 + DD_MS, "bouncing press settled at %d ms (want %d)", at, 5 + DD_MS);

    n = feed_dd("010010" "0000000000", &at);
    CHECK(n == 1, "bouncing release: %d changes (want 1)", n);
    CHECK(!dd_cooked(), "bouncing release settles up");

    char glitch[DD_MS + 12];
    memset(glitch, '0', sizeof(glitch) - 1);
    glitch[sizeof(glitch) - 1] = 0;
    memset(glitch + 2, '1', DD_MS - 1); // held one ms short of DD_MS
    n = feed_dd(glitch, NULL);
    CHECK(n == 0, "a %d ms glitch reported %d changes", DD_MS - 1, n);
}

// While Double Down bounces, optical keys on every row, the thumb row
// included, still change on the same scan.
static void test_dd_bounce_does_not_delay_optical(void) {
    reset(3000);
    const char *bounce = "1010110101101010";
    for (int i = 0; bounce[i]; i++) {
        raw[0] = (raw[0] & (matrix_row_t)~DD) | (bounce[i] == '1' ? DD : 0);
        int          r   = i % MATRIX_ROWS_PER_HAND;
        matrix_row_t bit = (matrix_row_t)1 << (i % DOUBLEDOWN_COL); // never the DD column
        raw[r] ^= bit;
        scan();
        CHECK((cooked[r] & bit) == (raw[r] & bit), "ms %d: optical r%d bit 0x%02x followed raw during bounce", i, r, bit);
        CHECK(!dd_cooked(), "ms %d: bouncing Double Down not yet reported", i);
        now_ms++;
    }
    // Then held down: it reports DD_MS later.
    raw[0] |= DD;
    int t = run_until_dd(true, 50);
    CHECK(t == DD_MS, "Double Down settled %d ms after the last bounce (want %d)", t, DD_MS);
}

// The wait is right across the 16-bit ms timer wrap.
static void test_timer_wrap(void) {
    reset((uint16_t)(65536 - 2));
    raw[0] = DD;
    int t  = run_until_dd(true, 50);
    CHECK(t == DD_MS, "press across the timer wrap after %d ms (want %d)", t, DD_MS);
}

int main(void) {
    test_optical_same_scan();
    test_dd_clean();
    test_dd_bounce();
    test_dd_bounce_does_not_delay_optical();
    test_timer_wrap();
    if (failures) {
        printf("sval_debounce: %d failures\n", failures);
        return 1;
    }
    printf("sval_debounce: all tests passed (Double Down %d ms)\n", DD_MS);
    return 0;
}
