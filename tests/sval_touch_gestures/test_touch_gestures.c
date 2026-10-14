// Host test of the firmware touch gesture recognizer (azoteq/touch_gestures.c)
// with synthetic contact traces. Run: util/test_multitouch_split.sh
//
// Replay mode: test_touch_gestures --replay TRACE [OWNERS_HEX]
// feeds a trace made by tools/multitouch/trace_from_log.py from a unified_lab
// --log recording and prints each output, so thresholds can be tuned offline.
#define SVAL_MT_HOST_TEST
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../keyboards/svalboard/azoteq/touch_gestures.c"

static uint16_t emitted[64];
static int      n_emitted;
static uint8_t  eeprom[TG_BLOCK_SIZE];
static int      eeprom_writes;
static bool     replay_print;

void tg_platform_emit(uint16_t keycode) {
    if (replay_print) printf("emit 0x%04X\n", keycode);
    if (n_emitted < 64) emitted[n_emitted] = keycode;
    n_emitted++;
}
void tg_storage_read(uint8_t block[TG_BLOCK_SIZE]) { memcpy(block, eeprom, TG_BLOCK_SIZE); }
void tg_storage_write(const uint8_t block[TG_BLOCK_SIZE]) { memcpy(eeprom, block, TG_BLOCK_SIZE); eeprom_writes++; }
uint8_t sval_mt_lease_mode(void) { return 2; }
uint8_t sval_mt_pad_contacts(uint8_t pad) { return pad_n(pad); }

// ---- trace helpers: contacts in millimeters on one pad (0-43 x, 0-40 y)

typedef struct { uint8_t id; double x, y; } touch_t;
static uint32_t now_ms;
static uint8_t  seq[2];

static void send(uint8_t pad, const touch_t *t, int n, uint8_t flags) {
    uint8_t f[56] = {0};
    f[0] = 1; f[1] = pad; f[2] = flags; f[4] = seq[pad]++;
    for (int i = 0; i < n && i < 5; i++) {
        if (!t[i].id) continue; // an empty slot: slots are stable on the chip
        uint8_t *c = f + 16 + 8 * i;
        uint16_t x = (uint16_t)(t[i].x * 2048 / 43), y = (uint16_t)(t[i].y * 1792 / 40);
        f[3] |= 1 << i;
        c[0] = t[i].id; c[1] = x & 0xFF; c[2] = x >> 8; c[3] = y & 0xFF; c[4] = y >> 8; c[5] = 10;
    }
    tg_feed(pad, f, now_ms);
}
static void up(void) { send(0, NULL, 0, 0); send(1, NULL, 0, 0); }
static void step(uint32_t ms) { now_ms += ms; }
// All fingers up, and past the drop grace, so the lift is real.
static void lift(void) { up(); step(TG_DROP_GRACE_MS + 1); tg_tick(now_ms); }

static void fresh(uint8_t owners) {
    tg_defaults(&tg_config);
    tg_config.owners = owners;
    tg_reset();
    n_emitted = 0;
    tg_take_count(); tg_take_claims();
    now_ms += 1000;
    up();
}

// Three fingers on one pad moving by (dx, dy) mm over `frames` 10 ms frames.
// Settle (40 ms) arms after four frames, so travel counts from the fifth.
static void swipe3(uint8_t pad, double dx, double dy, int frames) {
    double x0 = dx < 0 ? 30 : 8, y0 = dy < 0 ? 30 : dy > 0 ? 8 : 20;
    touch_t t[3] = {{1, x0, y0}, {2, x0 + 5, y0}, {3, x0 + 10, y0}};
    for (int k = 0; k <= frames; k++) {
        touch_t m[3];
        for (int i = 0; i < 3; i++) m[i] = (touch_t){t[i].id, t[i].x + dx * k / frames, t[i].y + dy * k / frames};
        send(pad, m, 3, 0); send(pad ^ 1, NULL, 0, 0);
        step(10);
    }
}

#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); exit(1); } } while (0)

static void test_windows_owner_is_inert(void) {
    fresh(TG_OPT_SPAN);
    CHECK(!tg_running());
    swipe3(1, -22, 0, 10);
    CHECK(n_emitted == 0 && !tg_claimed());
}

static void test_swipe_directions(void) {
    struct { double dx, dy; uint16_t kc; } cases[] = {
        {-20, 0, 0x094F}, {20, 0, 0x0950}, {0, -20, 0x082B}, {0, 20, 0x0807}};
    for (int i = 0; i < 4; i++) {
        fresh(TG_OWN_SWIPE_FW | TG_OPT_SPAN);
        swipe3(1, cases[i].dx, cases[i].dy, 12);
        CHECK(n_emitted == 1 && emitted[0] == cases[i].kc);
        CHECK(tg_claimed());
        lift(); CHECK(!tg_claimed());
        CHECK(tg_last_gesture() == i + 1);
    }
}

static void test_settle_and_staggered_landing(void) {
    fresh(TG_OWN_SWIPE_FW | TG_OPT_SPAN);
    // Fingers land 15 ms apart, then move; the travel before the third lands is not counted.
    touch_t a = {1, 10, 20}, b = {2, 15, 20}, c = {3, 20, 20};
    send(0, &a, 1, 0); step(15);
    touch_t ab[2] = {a, b}; send(0, ab, 2, 0); step(15);
    for (int k = 0; k < 30; k++) {
        touch_t m[3] = {{1, 10 + k * 0.6, 20}, {2, 15 + k * 0.6, 20}, {3, 20 + k * 0.6, 20}};
        (void)c;
        send(0, m, 3, 0); step(10);
    }
    CHECK(n_emitted == 1 && emitted[0] == 0x0950);
}

static void test_short_or_slow_or_diagonal_does_nothing(void) {
    fresh(TG_OWN_SWIPE_FW | TG_OPT_SPAN);
    swipe3(0, -8, 0, 10);            // under swipe_mm
    CHECK(n_emitted == 0);
    fresh(TG_OWN_SWIPE_FW | TG_OPT_SPAN);
    swipe3(0, -20, 0, 150);          // 12 mm after 900 ms: past the 600 ms window
    CHECK(n_emitted == 0);
    fresh(TG_OWN_SWIPE_FW | TG_OPT_SPAN);
    swipe3(0, -20, -18, 12);         // diagonal: fails the 2.0 axis ratio
    CHECK(n_emitted == 0);
    // Once the window ran out, a later fast move in the same touch still does nothing.
    fresh(TG_OWN_SWIPE_FW | TG_OPT_SPAN);
    touch_t t[3] = {{1, 15, 20}, {2, 20, 20}, {3, 25, 20}};
    for (int k = 0; k < 80; k++) { send(0, t, 3, 0); step(10); }
    swipe3(0, -20, 0, 5);
    CHECK(n_emitted == 0);
}

// A finger flickers out for 20 ms (under the 30 ms grace) and comes back with a
// new lifetime ID, at a different slot, mid-swipe: one swipe, right direction.
static void test_dropped_finger_mid_swipe_flicker(void) {
    fresh(TG_OWN_SWIPE_FW | TG_OPT_SPAN);
    for (int k = 0; k <= 20; k++) {
        double d = -1.2 * k;
        touch_t three[3] = {{1, 30 + d, 20}, {2, 35 + d, 20}, {3, 40 + d, 20}};
        touch_t two[3]   = {{1, 30 + d, 20}, {0, 0, 0}, {3, 40 + d, 20}};
        touch_t back[3]  = {{1, 30 + d, 20}, {9, 35 + d, 20}, {3, 40 + d, 20}};
        if (k == 4 || k == 5) send(0, two, 3, 0);
        else if (k > 5) send(0, back, 3, 0);
        else send(0, three, 3, 0);
        step(10);
    }
    CHECK(n_emitted == 1 && emitted[0] == 0x094F);
}

// A finger really lifts mid-swipe (sustain_min 1). The far-right finger
// leaving would jump a naive centroid 2.5 mm left; the rebase ignores it.
static void test_dropped_finger_mid_swipe_lift(void) {
    fresh(TG_OWN_SWIPE_FW | TG_OPT_SPAN);
    tg_config.swipe_mm = 6;
    for (int k = 0; k <= 24; k++) {
        double d = 0.45 * k; // moving right
        touch_t three[3] = {{1, 15 + d, 20}, {2, 20 + d, 20}, {3, 40 + d * 0.1, 20}};
        touch_t two[3]   = {{1, 15 + d, 20}, {2, 20 + d, 20}, {0, 0, 0}};
        if (k < 5) send(0, three, 3, 0); else send(0, two, 3, 0);
        step(10);
        // The far finger leaving would jump a naive centroid 7.5 mm left.
        if (k == 10) CHECK(n_emitted == 0);
    }
    CHECK(n_emitted == 1 && emitted[0] == 0x0950); // the remaining two finish the right swipe
}

static void test_sustain_and_repeat(void) {
    fresh(TG_OWN_SWIPE_FW | TG_OPT_SPAN);
    tg_config.swipe_mm = 6; tg_config.swipe_repeat_mm = 5;
    swipe3(1, -13, 0, 10); // fingers end at 17, 22, 27 mm
    CHECK(n_emitted == 1);
    // Two fingers lift; one keeps going 13 mm more: two repeats.
    for (int k = 1; k <= 13; k++) {
        touch_t one[3] = {{1, 17 - k, 30}, {0, 0, 0}, {0, 0, 0}};
        send(1, one, 3, 0); send(0, NULL, 0, 0); step(10);
    }
    CHECK(n_emitted == 3 && emitted[2] == 0x094F);
    // Reverse travel does nothing.
    for (int k = 0; k < 10; k++) { touch_t one = {1, 4 + k, 30}; send(1, &one, 1, 0); step(10); }
    CHECK(n_emitted == 3);
    lift();
    CHECK(!tg_claimed());
    // sustain_min 3: lifting to two fingers ends the gesture, with no repeats.
    fresh(TG_OWN_SWIPE_FW | TG_OPT_SPAN);
    tg_config.swipe_mm = 6; tg_config.swipe_repeat_mm = 5; tg_config.sustain_min = 3;
    swipe3(1, -13, 0, 10);
    for (int k = 1; k <= 20; k++) { touch_t two[2] = {{1, 17 - k * 0.5, 30}, {2, 22 - k * 0.5, 30}}; send(1, two, 2, 0); step(10); }
    CHECK(n_emitted == 1 && tg_claimed());
}

static void test_span_option(void) {
    // Two fingers on the left pad, one on the right, all moving left.
    for (int span = 0; span < 2; span++) {
        fresh(TG_OWN_SWIPE_FW | (span ? TG_OPT_SPAN : 0));
        for (int k = 0; k <= 24; k++) {
            touch_t l[2] = {{1, 30 - k, 20}, {2, 36 - k, 20}}, r = {1, 20 - k * 0.5, 20};
            send(0, l, 2, 0); send(1, &r, 1, 0); step(10);
        }
        CHECK(n_emitted == (span ? 1 : 0));
    }
}

static void pinch_frames(double a0, double b0, double a1, double b1, int frames, bool across) {
    for (int k = 0; k <= frames; k++) {
        double a = a0 + (a1 - a0) * k / frames, b = b0 + (b1 - b0) * k / frames;
        if (across) {
            touch_t l = {1, a, 20}, r = {1, b, 20};
            send(0, &l, 1, 0); send(1, &r, 1, 0);
        } else {
            touch_t t[2] = {{1, a, 20}, {2, b, 20}};
            send(1, t, 2, 0); send(0, NULL, 0, 0);
        }
        step(10);
    }
}

static void test_pinch(void) {
    fresh(TG_OWN_PINCH_FW | TG_OPT_SPAN);
    pinch_frames(15, 25, 9, 31, 12, false);   // spread 12 mm: start at 4, steps at 7 and 10
    CHECK(tg_claimed() && tg_pinch_active());
    CHECK(n_emitted == 2 && emitted[0] == 0x01D9 && emitted[1] == 0x01D9);
    pinch_frames(9, 31, 14, 26, 10, false);   // squeeze 10 mm: two steps in
    CHECK(n_emitted == 4 && emitted[3] == 0x01DA);
    // A third finger lands: still a pinch, never a swipe.
    for (int k = 0; k < 20; k++) { touch_t t[3] = {{1, 14, 20}, {2, 26, 20}, {3, 30 - k * 0.5, 10}}; send(1, t, 3, 0); step(10); }
    CHECK(tg_pinch_active() && n_emitted == 4);
    lift();
    CHECK(!tg_claimed() && !tg_pinch_active());
    // One finger on each pad (span), squeezing: pinch in.
    fresh(TG_OWN_PINCH_FW | TG_OPT_SPAN);
    pinch_frames(30, 13, 36, 7, 10, true);
    CHECK(n_emitted >= 1 && emitted[0] == 0x01DA);
    // Without span, one finger on each pad is two separate pads: nothing.
    fresh(TG_OWN_PINCH_FW);
    pinch_frames(30, 13, 36, 7, 10, true);
    CHECK(n_emitted == 0 && !tg_claimed());
}

static void test_scroll_is_not_pinch(void) {
    fresh(TG_OWN_PINCH_FW | TG_OPT_SPAN);
    for (int k = 0; k <= 20; k++) { touch_t t[2] = {{1, 15, 5 + k}, {2, 23, 5 + k * 1.1}}; send(0, t, 2, 0); send(1, NULL, 0, 0); step(10); }
    CHECK(n_emitted == 0 && !tg_claimed());
}

static void test_pinch_hold(void) {
    fresh(TG_OWN_PINCH_FW | TG_OPT_SPAN);
    touch_t one = {1, 15, 20}, two[2] = {{1, 15, 20}, {2, 25, 20}};
    send(0, &one, 1, 0); step(10);
    CHECK(!tg_pinch_hold(0, now_ms));
    send(0, two, 2, 0);
    CHECK(tg_pinch_hold(0, now_ms));
    CHECK(!tg_pinch_hold(1, now_ms));
    step(TG_PINCH_HOLD_MS); send(0, two, 2, 0);
    CHECK(!tg_pinch_hold(0, now_ms)); // released as scroll
    // Scroll owner chip but pinch owner Windows: never held.
    fresh(TG_OPT_SPAN | TG_OWN_SWIPE_FW);
    send(0, two, 2, 0);
    CHECK(!tg_pinch_hold(0, now_ms));
}

static void test_faults_and_staleness(void) {
    fresh(TG_OWN_SWIPE_FW | TG_OPT_SPAN);
    touch_t t[3] = {{1, 15, 20}, {2, 20, 20}, {3, 25, 20}};
    send(0, t, 3, 0); step(50); send(0, t, 3, 0);
    send(0, NULL, 0, 1); // palm: candidate dropped until all-up
    for (int k = 0; k < 10; k++) { touch_t m[3] = {{1, 15 - k * 2, 20}, {2, 20 - k * 2, 20}, {3, 25 - k * 2, 20}}; send(0, m, 3, 0); step(10); }
    CHECK(n_emitted == 0);
    // A claim survives a far pad that goes quiet, and ends when this pad lifts.
    fresh(TG_OWN_SWIPE_FW | TG_OPT_SPAN);
    swipe3(0, -22, 0, 10);
    CHECK(tg_claimed());
    step(TG_STALE_MS + 1); tg_tick(now_ms); // both pads stale: their contacts are gone
    CHECK(!tg_claimed());
}

static void test_config_and_command(void) {
    tg_config_t c, d;
    tg_defaults(&c);
    CHECK(!tg_validate(&c));
    uint8_t p[TG_PAYLOAD_SIZE];
    tg_encode(&c, p);
    CHECK(p[0] == 0x08 && p[1] == 0x13 && p[2] == 12 && p[3] == 20 && p[4] == 40 && p[5] == 60 && p[6] == 0 && p[7] == 8 && p[8] == 6);
    CHECK(p[9] == 0x4F && p[10] == 0x09 && p[17] == 0xD9 && p[19] == 0xDA && p[20] == 0x01);
    tg_decode(p, &d);
    CHECK(!memcmp(&c, &d, sizeof(c)));
    d.sustain_min = 4; CHECK(tg_validate(&d) == 2);
    d = c; d.swipe_repeat_mm = 2; CHECK(tg_validate(&d) == 2);
    d = c; d.owners = 0x20; CHECK(tg_validate(&d) == 2);

    // Bad stamp loads the defaults; a good one loads the stored values.
    memset(eeprom, 0xFF, sizeof(eeprom)); eeprom_writes = 0;
    tg_load(); CHECK(!memcmp(&tg_config, &c, sizeof(c)) && eeprom_writes == 0);

    uint8_t buf[59] = {0};
    buf[0] = 0; tg_command(buf, sizeof(buf));
    CHECK(buf[1] == 0 && buf[2] == 1 && buf[3] == 21 && !memcmp(buf + 4, p, 21));
    // SET swipe owner firmware: applies, saves once; the same SET again does not write.
    uint8_t q[21]; memcpy(q, p, 21); q[0] |= TG_OWN_SWIPE_FW; q[2] = 10;
    memset(buf, 0, sizeof(buf)); buf[0] = 1; buf[1] = 1; buf[2] = 21; memcpy(buf + 3, q, 21);
    tg_command(buf, sizeof(buf));
    CHECK(buf[1] == 0 && eeprom_writes == 1 && eeprom[0] == TG_STAMP && tg_config.swipe_mm == 10 && tg_running());
    memset(buf, 0, sizeof(buf)); buf[0] = 1; buf[1] = 1; buf[2] = 21; memcpy(buf + 3, q, 21);
    tg_command(buf, sizeof(buf));
    CHECK(buf[1] == 0 && eeprom_writes == 1);
    tg_load(); CHECK(tg_config.swipe_mm == 10);
    // Out of range: nothing applied.
    q[2] = 99;
    memset(buf, 0, sizeof(buf)); buf[0] = 1; buf[1] = 1; buf[2] = 21; memcpy(buf + 3, q, 21);
    tg_command(buf, sizeof(buf));
    CHECK(buf[1] == 2 && tg_config.swipe_mm == 10);
    // Wrong version.
    memset(buf, 0, sizeof(buf)); buf[0] = 1; buf[1] = 2; buf[2] = 21;
    tg_command(buf, sizeof(buf)); CHECK(buf[1] == 1);
    // DEFAULTS restores and saves.
    memset(buf, 0, sizeof(buf)); buf[0] = 2; tg_command(buf, sizeof(buf));
    CHECK(buf[1] == 0 && !memcmp(buf + 2, p, 21) && eeprom_writes == 2 && !tg_running());
    // STATUS reads and clears the counts.
    fresh(TG_OWN_SWIPE_FW | TG_OPT_SPAN);
    swipe3(1, -22, 0, 10);
    memset(buf, 0, sizeof(buf)); buf[0] = 3; tg_command(buf, sizeof(buf));
    CHECK(buf[1] == 0 && buf[2] == 2 && buf[3] == 1 && buf[4] == 1 && buf[5] == 1 && buf[7] == 1 && buf[9] == 0 && buf[10] == 3);
    memset(buf, 0, sizeof(buf)); buf[0] = 3; tg_command(buf, sizeof(buf));
    CHECK(buf[5] == 0 && buf[7] == 0);
}

static int replay(const char *path, const char *owners) {
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); return 2; }
    tg_defaults(&tg_config);
    tg_config.owners = owners ? (uint8_t)strtoul(owners, NULL, 16) : (TG_OWN_SWIPE_FW | TG_OWN_PINCH_FW | TG_OPT_SPAN);
    tg_reset();
    replay_print = true;
    char line[512];
    unsigned frames = 0;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#') continue;
        unsigned long ms; unsigned pad, flags, mask;
        int used = 0;
        if (sscanf(line, "%lu %u %u %u%n", &ms, &pad, &flags, &mask, &used) != 4) continue;
        uint8_t fr[56] = {0};
        fr[0] = 1; fr[1] = pad; fr[2] = flags; fr[3] = mask;
        char *p = line + used;
        for (int i = 0; i < 5; i++) {
            unsigned id, x, y; int n = 0;
            if (sscanf(p, "%u %u %u%n", &id, &x, &y, &n) != 3) break;
            p += n;
            uint8_t *c = fr + 16 + 8 * i;
            c[0] = id; c[1] = x & 0xFF; c[2] = x >> 8; c[3] = y & 0xFF; c[4] = y >> 8;
        }
        bool was = tg_claimed();
        tg_feed(pad, fr, (uint32_t)ms);
        if (tg_claimed() != was) printf("%lu ms: %s\n", ms, tg_claimed() ? "claim" : "release");
        frames++;
    }
    fclose(f);
    printf("%u frames, %d outputs\n", frames, n_emitted);
    return 0;
}

int main(int argc, char **argv) {
    if (argc >= 3 && !strcmp(argv[1], "--replay")) return replay(argv[2], argc > 3 ? argv[3] : NULL);
    test_windows_owner_is_inert();
    test_swipe_directions();
    test_settle_and_staggered_landing();
    test_short_or_slow_or_diagonal_does_nothing();
    test_dropped_finger_mid_swipe_flicker();
    test_dropped_finger_mid_swipe_lift();
    test_sustain_and_repeat();
    test_span_option();
    test_pinch();
    test_scroll_is_not_pinch();
    test_pinch_hold();
    test_faults_and_staleness();
    test_config_and_command();
    puts("touch gestures: owners, four swipe directions, settle, thresholds, window, ratio, dropped finger (flicker and lift), sustain and repeat, span, pinch in/out, scroll vs pinch, pinch hold, faults, staleness, payload and 0x2E sub-commands passed");
    return 0;
}
