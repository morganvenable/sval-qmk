// Host test for pointing_motion.h: the detected-code to fragment mapping (Sval 0x18) and the
// POINTING_MOTION (Sval 0x2D) packing, clamping and saturation, and the per-reader counts. sval_config.h is generated
// from the real sval.json by test_detect.py.
#include <stdio.h>
#include <string.h>
#include "sval_config.h"
#include "pointing_motion.h"

static int failures = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            failures++;                                                 \
        }                                                               \
    } while (0)

static int16_t le16(const uint8_t *p) { return (int16_t)(p[0] | (p[1] << 8)); }

static void test_mapping(void) {
    CHECK(SVAL_FRAGMENT_IDX_LEFT_POINTING == 10);
    CHECK(SVAL_FRAGMENT_IDX_RIGHT_POINTING == 11);
    CHECK(sval_pointing_fragment_for_detected(0) == 0xFF);
    CHECK(sval_pointing_fragment_for_detected(1) == 5);
    CHECK(sval_pointing_fragment_for_detected(2) == 6);
    CHECK(sval_pointing_fragment_for_detected(3) == 4);
    CHECK(sval_pointing_fragment_for_detected(4) == 9);
    CHECK(sval_pointing_fragment_for_detected(5) == 7);
    CHECK(sval_pointing_fragment_for_detected(6) == 8);
    CHECK(sval_pointing_fragment_for_detected(7) == 0xFF);
    CHECK(sval_pointing_fragment_for_detected(0xFF) == 0xFF);
}

static void test_pack_plain(void) {
    sval_motion_acc_t acc = {0};
    uint8_t           out[SVAL_MOTION_HAND_LEN];
    sval_motion_accumulate(&acc, 3, -4, 1, -2, 0x01);
    sval_motion_accumulate(&acc, -1, -6, 0, 5, 0x04);
    sval_motion_pack_hand(out, &acc, SVAL_MOTION_VALID | SVAL_MOTION_SCROLLING);
    CHECK(out[0] == (SVAL_MOTION_VALID | SVAL_MOTION_SCROLLING));
    CHECK(le16(&out[1]) == 2);
    CHECK(le16(&out[3]) == -10);
    CHECK((int8_t)out[5] == 1);
    CHECK((int8_t)out[6] == 3);
    CHECK(out[7] == 0x05);
    CHECK(out[8] == 0 && out[9] == 0);
}

static void test_pack_clamps(void) {
    sval_motion_acc_t acc = {0};
    uint8_t           out[SVAL_MOTION_HAND_LEN];
    for (int i = 0; i < 400; i++) sval_motion_accumulate(&acc, 127, -127, 0, 0, 0);
    CHECK(!acc.saturated); // the int32 accumulators hold it
    sval_motion_pack_hand(out, &acc, SVAL_MOTION_VALID);
    CHECK(le16(&out[1]) == 32767);
    CHECK(le16(&out[3]) == -32768);
    CHECK(out[0] == (SVAL_MOTION_VALID | SVAL_MOTION_SATURATED));

    memset(&acc, 0, sizeof(acc));
    sval_motion_accumulate(&acc, 0, 0, 100, -100, 0);
    sval_motion_accumulate(&acc, 0, 0, 100, -100, 0);
    sval_motion_pack_hand(out, &acc, SVAL_MOTION_VALID);
    CHECK((int8_t)out[5] == 127);
    CHECK((int8_t)out[6] == -128);
    CHECK(out[0] & SVAL_MOTION_SATURATED);

    // int16 h/v accumulators saturate rather than wrap
    memset(&acc, 0, sizeof(acc));
    for (int i = 0; i < 300; i++) sval_motion_accumulate(&acc, 0, 0, 127, -128, 0);
    CHECK(acc.h == 32767 && acc.v == -32768 && acc.saturated);

    // int32 accumulators saturate rather than wrap
    memset(&acc, 0, sizeof(acc));
    acc.x = INT32_MAX - 1;
    acc.y = INT32_MIN + 1;
    sval_motion_accumulate(&acc, 5, -5, 0, 0, 0);
    CHECK(acc.x == INT32_MAX && acc.y == INT32_MIN && acc.saturated);
    sval_motion_pack_hand(out, &acc, 0);
    CHECK(out[0] == SVAL_MOTION_SATURATED);

    // bit 7 is always zero
    memset(&acc, 0, sizeof(acc));
    sval_motion_pack_hand(out, &acc, 0xFF);
    CHECK((out[0] & 0x80) == 0);
}

static void test_elapsed(void) {
    CHECK(sval_motion_elapsed(true, 1234) == 0);
    CHECK(sval_motion_elapsed(false, 33) == 33);
    CHECK(sval_motion_elapsed(false, 255) == 255);
    CHECK(sval_motion_elapsed(false, 100000) == 255);
}

#define ACTIVE_MS 1500

// One 0x2D read by reader `id` at `now`, as svalboard.c does it: the left hand's dx.
static int32_t read_dx(sval_motion_reader_t *t, uint8_t id, uint32_t now, uint8_t *elapsed) {
    uint8_t               out[SVAL_MOTION_HAND_LEN];
    uint8_t               e;
    sval_motion_reader_t *r = sval_motion_reader_begin_read(t, SVAL_MOTION_READERS, id, now, &e);
    sval_motion_pack_hand(out, &r->acc[0], SVAL_MOTION_VALID);
    memset(r->acc, 0, sizeof(r->acc));
    if (elapsed) *elapsed = e;
    return le16(&out[1]);
}

static void move(sval_motion_reader_t *t, uint32_t now, int32_t dx) {
    if (!sval_motion_any_live(t, SVAL_MOTION_READERS, now, ACTIVE_MS)) return;
    sval_motion_readers_accumulate(t, SVAL_MOTION_READERS, now, ACTIVE_MS, 0, dx, 0, 0, 0, 0);
    sval_motion_readers_accumulate(t, SVAL_MOTION_READERS, now, ACTIVE_MS, 1, -dx, 0, 0, 0, 0);
}

// Two readers polling at different rates (8 ms and 13 ms) each get the full sum.
static void test_readers_full_sum(void) {
    sval_motion_reader_t t[SVAL_MOTION_READERS];
    memset(t, 0, sizeof(t));
    uint8_t e;
    CHECK(read_dx(t, 1, 0, &e) == 0 && e == 0);
    CHECK(read_dx(t, 2, 0, &e) == 0 && e == 0);
    int32_t sum1 = 0, sum2 = 0, moved = 0;
    for (uint32_t ms = 1; ms <= 1000; ms++) {
        move(t, ms, 3);
        moved += 3;
        if (ms % 8 == 0) sum1 += read_dx(t, 1, ms, NULL);
        if (ms % 13 == 0) sum2 += read_dx(t, 2, ms, NULL);
    }
    sum1 += read_dx(t, 1, 1000, NULL);
    sum2 += read_dx(t, 2, 1000, &e);
    CHECK(moved == 3000);
    CHECK(sum1 == moved);
    CHECK(sum2 == moved);
    CHECK(e == 12); // last read at 988 ms
    CHECK(read_dx(t, 2, 1010, &e) == 0 && e == 10);
    // the right hand's counts are kept apart from the left's
    move(t, 1011, 5);
    sval_motion_reader_t *r = sval_motion_reader_begin_read(t, SVAL_MOTION_READERS, 1, 1012, &e);
    CHECK(r->acc[0].x == 5 && r->acc[1].x == -5);
}

// A client that names no reader (id 0) behaves as before: one read-and-clear stream.
static void test_reader_legacy(void) {
    sval_motion_reader_t t[SVAL_MOTION_READERS];
    memset(t, 0, sizeof(t));
    uint8_t e;
    move(t, 1, 50); // nobody reading: not counted
    CHECK(read_dx(t, 0, 2, &e) == 0 && e == 0);
    move(t, 3, 7);
    move(t, 4, 8);
    CHECK(read_dx(t, 0, 10, &e) == 15 && e == 8);
    CHECK(read_dx(t, 0, 20, &e) == 0 && e == 10);
    // a second legacy client shares slot 0, as with older firmware
    move(t, 21, 4);
    CHECK(read_dx(t, 0, 22, NULL) == 4);
    CHECK(read_dx(t, 0, 23, NULL) == 0);
    // a named reader alongside it doesn't take from it
    CHECK(read_dx(t, 1, 24, NULL) == 0);
    move(t, 25, 9);
    CHECK(read_dx(t, 1, 26, NULL) == 9);
    CHECK(read_dx(t, 0, 27, NULL) == 9);
}

// A reader stops counting 1.5 s after its last read; its slot can then go to another.
static void test_reader_expiry(void) {
    sval_motion_reader_t t[SVAL_MOTION_READERS];
    memset(t, 0, sizeof(t));
    uint8_t e;
    read_dx(t, 1, 0, NULL);
    move(t, ACTIVE_MS - 1, 2);
    move(t, ACTIVE_MS, 100); // expired: not counted
    CHECK(!sval_motion_any_live(t, SVAL_MOTION_READERS, ACTIVE_MS, ACTIVE_MS));
    CHECK(read_dx(t, 1, 3000, &e) == 2 && e == 255);
    move(t, 3001, 1);
    CHECK(read_dx(t, 1, 3002, &e) == 1 && e == 2);

    // fill every slot; a new reader takes the one read longest ago
    memset(t, 0, sizeof(t));
    for (uint8_t id = 1; id <= SVAL_MOTION_READERS; id++) read_dx(t, id, id, NULL);
    move(t, 10, 6);
    CHECK(read_dx(t, 9, 11, &e) == 0 && e == 0); // evicts reader 1
    bool found1 = false, found9 = false;
    for (int i = 0; i < SVAL_MOTION_READERS; i++) {
        if (t[i].used && t[i].id == 1) found1 = true;
        if (t[i].used && t[i].id == 9) found9 = true;
    }
    CHECK(!found1 && found9);
    CHECK(read_dx(t, 2, 12, NULL) == 6); // the others kept their counts
    CHECK(read_dx(t, SVAL_MOTION_READERS, 12, NULL) == 6);
    // reader 1 comes back fresh: a new slot, nothing carried over
    CHECK(read_dx(t, 1, 13, &e) == 0 && e == 0);

    // timer wrap
    memset(t, 0, sizeof(t));
    read_dx(t, 1, UINT32_MAX - 5, NULL);
    move(t, 4, 3);
    CHECK(read_dx(t, 1, 6, &e) == 3 && e == 12);
}

static void test_put16(void) {
    uint8_t b[2];
    sval_motion_put16(b, 1600);
    CHECK(b[0] == 0x40 && b[1] == 0x06);
}

static void test_layout(void) {
    CHECK(SVAL_MOTION_LEFT_OFF + SVAL_MOTION_HAND_LEN == SVAL_MOTION_RIGHT_OFF);
    CHECK(SVAL_MOTION_RIGHT_OFF + SVAL_MOTION_HAND_LEN == SVAL_MOTION_AXIS_OFF);
    CHECK(SVAL_MOTION_AXIS_OFF + 1 == SVAL_MOTION_REPORT_LEN);
    CHECK(2 + SVAL_MOTION_REPORT_LEN <= 26); // fits a client-wrapped 32-byte packet
    CHECK(SVAL_MOTION_READER_OFF == SVAL_MOTION_REPORT_LEN);
    CHECK(SVAL_MOTION_LEFT_CPI_OFF == SVAL_MOTION_READER_OFF + 1);
    CHECK(SVAL_MOTION_RIGHT_CPI_OFF == SVAL_MOTION_LEFT_CPI_OFF + 2);
    CHECK(SVAL_MOTION_EXT_LEN == SVAL_MOTION_RIGHT_CPI_OFF + 2);
    CHECK(2 + SVAL_MOTION_EXT_LEN <= 64 - 5); // the extension fits a wrapped 64-byte packet
}

int main(void) {
    test_mapping();
    test_pack_plain();
    test_pack_clamps();
    test_elapsed();
    test_layout();
    test_readers_full_sum();
    test_reader_legacy();
    test_reader_expiry();
    test_put16();
    if (failures) return 1;
    printf("OK\n");
    return 0;
}
