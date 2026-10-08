// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

// Host tests for the updater's session state machine (updater.c) and the
// confirmation chord (update_gesture.c), with simulated time. Included by
// test_updater.c after its flash mock, whose ROM operations advance sim_us.
//
// Each updater_task() call is one main-loop pass; the test adds 1 ms of other
// work between passes. "Every op answers within 50 ms" is checked as: the
// longest single pass plus the longest op handler, in simulated flash time,
// stays at or under 50 ms. The signature check is one call into Monocypher and
// cannot be timed on the host; its pass is tracked separately.

#include "updater.h"
#include "updater_port.h"
#include "update_gesture.h"
#include "update_led.h"
#include "update_commit.h"

void updater_host_reset(void);

// ---- port and mocks ----------------------------------------------------------------------

static uint32_t        port_client; // 0: the packet came without the client wrapper
static bool            port_failing;
static uint32_t        port_rand = 0x2545F491u;
static update_device_t self_dev;

uint32_t updater_port_now_ms(void) {
    return (uint32_t)(sim_us / 1000);
}
bool updater_port_client(uint32_t *id) {
    if (!port_client) return false;
    *id = port_client;
    return true;
}
bool updater_port_settings_failing(void) {
    return port_failing;
}
uint32_t updater_port_random32(void) {
    port_rand = port_rand * 1664525u + 1013904223u;
    return port_rand ? port_rand : 1;
}
void update_device_self(update_device_t *dev) {
    *dev = self_dev;
}

static update_led_mode_t led_now;
static int               led_seen; // bit per mode shown
void update_led_show(update_led_mode_t mode) {
    led_now = mode;
    led_seen |= 1 << mode;
}

// A mock commit, so the COMMITTING path can be tested on its own. The real
// update_commit.c is tested by test_commit.c, in the build of this harness
// with -DSVAL_TEST_REAL_COMMIT.
static bool            commit_avail;
static int             commit_runs;
static uint32_t        commit_crc, commit_len;
static update_status_t commit_result;
#ifndef SVAL_TEST_REAL_COMMIT
bool update_commit_available(void) {
    return commit_avail;
}
update_status_t update_commit_run(const sval_update_manifest_t *m, uint32_t crc_body) {
    commit_runs++;
    commit_crc = crc_body;
    commit_len = m->image_len;
    return commit_result;
}
#endif

// ---- driving it -------------------------------------------------------------------------

#define CLIENT_A 0x00A1A1A1u
#define CLIENT_B 0x00B2B2B2u
#define CLIENT_C 0x00C3C3C3u

static uint8_t  rsp[23];
static uint8_t  req_hand = UPDATE_HAND_RIGHT;
static uint8_t  req_cmd  = 0x07; // id_custom_set_value
static uint64_t max_op_us, max_pass_us, sig_pass_us;

static uint32_t get24le(const uint8_t *p) {
    return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}
static uint32_t get32le(const uint8_t *p) {
    return get24le(p) | ((uint32_t)p[3] << 24);
}
static uint16_t get16le(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static update_status_t send(uint32_t client, uint8_t opcode, const void *args, size_t n) {
    uint8_t pkt[32];
    memset(pkt, 0xEE, sizeof(pkt)); // stale bytes past the request
    pkt[0] = req_cmd;
    pkt[1] = UPDATE_CHANNEL;
    pkt[2] = opcode;
    pkt[3] = req_hand;
    memset(pkt + 4, 0, 22);
    if (n) memcpy(pkt + 4, args, n);
    port_client = client;
    uint64_t t0 = sim_us;
    updater_via_command(pkt, sizeof(pkt));
    if (sim_us - t0 > max_op_us) max_op_us = sim_us - t0;
    port_client = 0;
    CHECK(pkt[0] == req_cmd && pkt[1] == UPDATE_CHANNEL && pkt[2] == opcode); // header untouched
    for (int i = 26; i < 32; i++) CHECK(pkt[i] == 0xEE);                    // nothing past the 23 value bytes
    memcpy(rsp, pkt + 3, sizeof(rsp));
    return (update_status_t)rsp[0];
}

static update_status_t send_nonce(uint32_t client, uint8_t opcode, uint32_t nonce) {
    uint8_t a[4];
    put32(a, nonce);
    return send(client, opcode, a, 4);
}

static void pass(void) {
    update_state_t before = updater_state();
    uint64_t       t0     = sim_us;
    updater_task();
    uint64_t dt = sim_us - t0;
    if (before == UPDATE_STATE_VERIFYING_MANIFEST) {
        if (dt > sig_pass_us) sig_pass_us = dt;
    } else if (dt > max_pass_us) {
        max_pass_us = dt;
    }
    sim_us += 1000;
}

static void wait_ms(uint32_t ms) {
    for (uint32_t i = 0; i < ms; i++) pass();
}

static void passes_until(update_state_t want, int max) {
    for (int i = 0; i < max && updater_state() != want && updater_state() != UPDATE_STATE_ERROR; i++) pass();
}

// ---- the chord ---------------------------------------------------------------------------

static matrix_row_t rows[5];

// One matrix scan with the chord keys at (a, b) and an unrelated key held;
// returns what QMK would see in rows 1 and 2, column 0.
static uint8_t scan(bool a, bool b) {
    rows[0] = 0x04;
    rows[SVAL_UPDATE_CHORD_ROW_A] = (a ? 1 : 0) << SVAL_UPDATE_CHORD_COL | 0x08;
    rows[SVAL_UPDATE_CHORD_ROW_B] = (b ? 1 : 0) << SVAL_UPDATE_CHORD_COL;
    update_gesture_scan(rows);
    CHECK(rows[0] == 0x04 && (rows[SVAL_UPDATE_CHORD_ROW_A] & 0x08)); // other keys pass through
    return (rows[SVAL_UPDATE_CHORD_ROW_A] & 1) | (rows[SVAL_UPDATE_CHORD_ROW_B] & 1) << 1;
}

// Hold the chord for ms, scanning every 10 ms, with main-loop passes.
static bool hold(uint32_t ms) {
    bool leaked = false;
    for (uint32_t t = 0; t < ms; t += 10) {
        leaked |= scan(true, true) != 0;
        for (int i = 0; i < 10; i++) pass();
    }
    return leaked;
}

static void chord(void) {
    scan(false, false);
    CHECK(!hold(SVAL_UPDATE_CHORD_HOLD_MS + 50));
    scan(false, false);
}

// ---- a signed update ------------------------------------------------------------------------

#define IMG_MAX SVAL_UPDATE_MAX_IMAGE
static uint8_t  img[IMG_MAX];
static uint8_t  blob[UPDATE_SIGNED_MANIFEST_BYTES];
static uint32_t img_len;
static uint8_t  sk[64];

static const char test_seed_hex[] = "c7a0ee43353475bc4715855ca7769c8eb4b7148a7641a1faddd9272a59e87f41"; // tools/sval_update_TEST_ONLY.key

static sval_update_manifest_t *blob_manifest(void) {
    return (sval_update_manifest_t *)blob;
}

static void sign_blob(void) {
    crypto_ed25519_sign(blob + UPDATE_MANIFEST_BYTES, sk, blob, UPDATE_MANIFEST_BYTES);
}

static void make_update(uint32_t len) {
    img_len = len;
    for (uint32_t i = 0; i < len; i++) img[i] = (uint8_t)(i * 13 + 5 + (i >> 9));
    memcpy(img, boot2_page0, 256);
    put32(img + 0x100, 0x20041000);
    put32(img + 0x104, 0x100001F7);
    sval_update_manifest_t m = good_manifest();
    m.image_len              = len;
    m.security_epoch         = 0;
    crypto_sha512(m.sha512, img, len);
    memcpy(blob, &m, sizeof(m));
    sign_blob();
}

static uint32_t body_crc(void) {
    return update_crc32_mpeg2(img + 0x100, img_len - 0x100);
}

static update_status_t load_manifest(uint32_t client) {
    for (uint32_t off = 0; off < UPDATE_SIGNED_MANIFEST_BYTES; off += 20) {
        uint8_t n = UPDATE_SIGNED_MANIFEST_BYTES - off < 20 ? (uint8_t)(UPDATE_SIGNED_MANIFEST_BYTES - off) : 20;
        uint8_t a[22];
        a[0] = (uint8_t)off;
        a[1] = n;
        memcpy(a + 2, blob + off, n);
        update_status_t st = send(client, UPDATE_OP_MANIFEST, a, 2 + n);
        if (st != UPDATE_OK) return st;
        CHECK_EQ(rsp[1], off + n);
    }
    return UPDATE_OK;
}

static update_status_t send_chunk(uint32_t client, uint32_t off, uint8_t n, const uint8_t *data) {
    uint8_t a[4 + 18] = {0};
    a[0] = off & 0xFF;
    a[1] = (off >> 8) & 0xFF;
    a[2] = (off >> 16) & 0xFF;
    a[3] = n;
    memcpy(a + 4, data, n > 18 ? 18 : n);
    return send(client, UPDATE_OP_CHUNK, a, sizeof(a));
}

static update_status_t send_image(uint32_t client, uint32_t from, uint32_t to) {
    for (uint32_t off = from; off < to;) {
        uint8_t         n  = to - off < 18 ? (uint8_t)(to - off) : 18;
        update_status_t st = send_chunk(client, off, n, img + off);
        if (st != UPDATE_OK) return st;
        CHECK_EQ(get24le(rsp + 1), off + n);
        off += n;
    }
    return UPDATE_OK;
}

static uint32_t session_nonce;

static bool to_confirm_wait(uint32_t client) {
    if (load_manifest(client) != UPDATE_OK) return false;
    if (send(client, UPDATE_OP_ARM, NULL, 0) != UPDATE_OK) return false;
    session_nonce = get32le(rsp + 1);
    return updater_state() == UPDATE_STATE_CONFIRM_WAIT;
}

static bool to_receiving(uint32_t client) {
    if (!to_confirm_wait(client)) return false;
    chord();
    if (send_nonce(client, UPDATE_OP_BEGIN, session_nonce) != UPDATE_ACCEPTED) return false;
    passes_until(UPDATE_STATE_RECEIVING, 2000);
    return updater_state() == UPDATE_STATE_RECEIVING;
}

static bool to_verified(uint32_t client) {
    if (!to_receiving(client)) return false;
    if (send_image(client, 0, img_len) != UPDATE_OK) return false;
    if (send_nonce(client, UPDATE_OP_END, session_nonce) != UPDATE_ACCEPTED) return false;
    passes_until(UPDATE_STATE_VERIFIED, 5000);
    return updater_state() == UPDATE_STATE_VERIFIED;
}

static void fresh(uint32_t len) {
    updater_host_reset();
    die_reset();
    scan(false, false);
    scan(false, false);
    port_failing  = false;
    commit_avail  = false;
    commit_runs   = 0;
    commit_result = UPDATE_UNSUPPORTED;
    req_hand      = UPDATE_HAND_RIGHT;
    req_cmd       = 0x07;
    self_dev      = test_dev;
    self_dev.security_epoch = 0;
    make_update(len);
}

// ---- tests ---------------------------------------------------------------------------------

static void test_session_info_and_wrapper(void) {
    fresh(0x3000);
    // INFO needs no wrapper, and takes 0xFF for "this half"
    CHECK_EQ(send(0, UPDATE_OP_INFO, NULL, 0), UPDATE_OK);
    CHECK_EQ(rsp[1], UPDATE_STATE_IDLE);
    CHECK_EQ(rsp[2], UPDATE_PROTOCOL_VERSION);
    CHECK_EQ(get16le(rsp + 3), SVAL_UPDATE_BASE / 4096);
    CHECK_EQ(get16le(rsp + 5), SVAL_UPDATE_SIZE / 4096);
    CHECK_EQ(get16le(rsp + 7), SVAL_UPDATE_MAX_IMAGE / 4096);
    CHECK(rsp[9] == 0xEF && rsp[10] == 0x40 && rsp[11] == 0x18);
    CHECK_EQ(rsp[12], UPDATE_POINTING_PMW3389);
    CHECK_EQ(rsp[13], UPDATE_HAND_RIGHT);
    CHECK_EQ(rsp[18], SVAL_UPDATE_STORAGE_FORMAT);
    CHECK_EQ(rsp[21], 0x01);
    req_hand = 0xFF;
    CHECK_EQ(send(0, UPDATE_OP_INFO, NULL, 0), UPDATE_OK);
    CHECK_EQ(rsp[13], UPDATE_HAND_RIGHT);
    // the other half, or 0xFF for anything but INFO: UNSUPPORTED (M1 has no relay)
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_STATUS, NULL, 0), UPDATE_UNSUPPORTED);
    req_hand = UPDATE_HAND_LEFT;
    CHECK_EQ(send(0, UPDATE_OP_INFO, NULL, 0), UPDATE_UNSUPPORTED);
    for (uint8_t op = UPDATE_OP_MANIFEST; op <= UPDATE_OP_REBIND; op++) CHECK_EQ(send(CLIENT_A, op, NULL, 0), UPDATE_UNSUPPORTED);
    req_hand = UPDATE_HAND_RIGHT;

    // every op but INFO refused without the client wrapper, with no state change
    uint8_t a[22] = {0, 20};
    memcpy(a + 2, blob, 20);
    for (uint8_t op = UPDATE_OP_MANIFEST; op <= UPDATE_OP_REBIND; op++) {
        CHECK_EQ(send(0, op, a, sizeof(a)), UPDATE_INVALID);
        CHECK_EQ(updater_state(), UPDATE_STATE_IDLE);
    }
    // unknown ops, wrapped or not
    CHECK_EQ(send(CLIENT_A, 0x0A, NULL, 0), UPDATE_INVALID);
    CHECK_EQ(send(CLIENT_A, 0xFF, NULL, 0), UPDATE_INVALID);
    // VIA's save command on this channel is left alone (value byte 0 is still the request's hand)
    req_cmd = 0x09;
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_MANIFEST, a, sizeof(a)), UPDATE_HAND_RIGHT);
    CHECK_EQ(updater_state(), UPDATE_STATE_IDLE);
    req_cmd = 0x08; // get_value works like set_value
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_MANIFEST, a, sizeof(a)), UPDATE_OK);
    req_cmd = 0x07;
    CHECK_EQ(updater_state(), UPDATE_STATE_MANIFEST_LOADING);
    // mid-session, unwrapped packets still change nothing
    CHECK(to_receiving(CLIENT_A));
    CHECK_EQ(send_chunk(0, 0, 18, img), UPDATE_INVALID);
    CHECK_EQ(send_nonce(0, UPDATE_OP_ABORT, session_nonce), UPDATE_INVALID);
    CHECK_EQ(send_nonce(0, UPDATE_OP_REBIND, session_nonce), UPDATE_INVALID);
    CHECK_EQ(updater_state(), UPDATE_STATE_RECEIVING);
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_STATUS, NULL, 0), UPDATE_OK);
    CHECK_EQ(get24le(rsp + 2), 0);
    // a short report is refused, not read past its end
    uint8_t pkt[32] = {0x07, UPDATE_CHANNEL, UPDATE_OP_INFO, 0xFF};
    port_client     = CLIENT_A;
    updater_via_command(pkt, 20);
    port_client = 0;
    CHECK_EQ(pkt[3], UPDATE_INVALID);
}

static void test_session_happy_path(void) {
    fresh(0x30000); // 48 sectors, across three 64 KiB blocks
    memset(update_host_flash + SVAL_UPDATE_BASE, 0x00, SVAL_UPDATE_SIZE); // every sector needs erasing
    snap();
    max_op_us = max_pass_us = sig_pass_us = 0;
    led_seen                              = 0;

    // record each distinct state, in order
    update_state_t seen[16];
    int            nseen = 0;
#define NOTE()                                                                        \
    do {                                                                              \
        if (nseen == 0 || seen[nseen - 1] != updater_state()) seen[nseen++] = updater_state(); \
    } while (0)
    NOTE();
    CHECK_EQ(load_manifest(CLIENT_A), UPDATE_OK);
    NOTE();
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_ARM, NULL, 0), UPDATE_OK);
    NOTE();
    session_nonce = get32le(rsp + 1);
    CHECK(session_nonce != 0);
    uint8_t h[64];
    crypto_sha512(h, blob, UPDATE_MANIFEST_BYTES);
    CHECK(memcmp(rsp + 5, h, 4) == 0); // the hash prefix the host shows (D5)
    pass();
    CHECK_EQ(led_now, UPDATE_LED_CONFIRM);
    CHECK_EQ(send_nonce(CLIENT_A, UPDATE_OP_BEGIN, session_nonce), UPDATE_NOT_CONFIRMED); // no chord yet: refused, still waiting
    CHECK_EQ(updater_state(), UPDATE_STATE_CONFIRM_WAIT);
    chord();
    pass();
    CHECK_EQ(led_now, UPDATE_LED_CHORD);
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_STATUS, NULL, 0), UPDATE_OK);
    CHECK_EQ(rsp[21] & 0x03, 0x03);
    wait_ms(400);
    CHECK_EQ(led_now, UPDATE_LED_APPROVED);
    CHECK_EQ(send_nonce(CLIENT_A, UPDATE_OP_BEGIN, session_nonce), UPDATE_ACCEPTED);
    NOTE();
    // CHUNK and END wait for the erase
    CHECK_EQ(send_chunk(CLIENT_A, 0, 18, img), UPDATE_BUSY);
    CHECK_EQ(send_nonce(CLIENT_A, UPDATE_OP_END, session_nonce), UPDATE_BUSY);
    for (int i = 0; i < 2000 && updater_state() != UPDATE_STATE_RECEIVING; i++) {
        pass();
        NOTE();
        CHECK(updater_state() != UPDATE_STATE_ERROR);
    }
    CHECK_EQ(led_now, UPDATE_LED_PROGRESS);
    CHECK_EQ(sector_erases, 0x30000 / 4096);
    CHECK_EQ(block_erases, 0);
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_STATUS, NULL, 0), UPDATE_OK);
    CHECK_EQ(get16le(rsp + 8), 48);
    CHECK_EQ(get16le(rsp + 10), 48);
    CHECK_EQ(get16le(rsp + 17), SIM_SECTOR_ERASE_US / 1000);
    CHECK_EQ(send_image(CLIENT_A, 0, img_len), UPDATE_OK);
    CHECK_EQ(send_nonce(CLIENT_A, UPDATE_OP_END, session_nonce), UPDATE_ACCEPTED);
    NOTE();
    for (int i = 0; i < 5000 && updater_state() == UPDATE_STATE_VERIFYING_IMAGE; i++) pass();
    NOTE();
    CHECK_EQ(updater_state(), UPDATE_STATE_VERIFIED);
    CHECK(memcmp(update_host_flash + SVAL_UPDATE_BASE, img, img_len) == 0);
    CHECK(unchanged_outside(SVAL_UPDATE_BASE, SVAL_UPDATE_BASE + SVAL_UPDATE_SIZE)); // invariant (a)
    CHECK_EQ(outside_ops, 0);
    CHECK_EQ(sequence_errors, 0);
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_STATUS, NULL, 0), UPDATE_OK);
    CHECK_EQ(rsp[1], UPDATE_STATE_VERIFIED);
    CHECK_EQ(get24le(rsp + 2), img_len);
    CHECK_EQ(get24le(rsp + 5), img_len);
    CHECK_EQ(get16le(rsp + 19), body_crc() & 0xFFFF);
    CHECK_EQ(rsp[12], UPDATE_OK);

    // COMMIT: the stub refuses and nothing happens
    uint8_t c[6];
    put32(c, session_nonce);
    c[4] = body_crc() & 0xFF;
    c[5] = (body_crc() >> 8) & 0xFF;
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_COMMIT, c, 6), UPDATE_UNSUPPORTED);
    CHECK_EQ(updater_state(), UPDATE_STATE_VERIFIED);
    // with a commit routine present: acknowledged, run 100 ms later from a pass
    commit_avail  = true;
    commit_result = UPDATE_BUSY; // e.g. a DMA channel busy at step 1: refused with no erase
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_COMMIT, c, 6), UPDATE_ACCEPTED);
    NOTE();
    CHECK_EQ(updater_state(), UPDATE_STATE_COMMITTING);
    // nothing but INFO and STATUS while committing
    CHECK_EQ(send_nonce(CLIENT_A, UPDATE_OP_ABORT, session_nonce), UPDATE_BUSY);
    CHECK_EQ(send_nonce(CLIENT_A, UPDATE_OP_REBIND, session_nonce), UPDATE_BUSY);
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_STATUS, NULL, 0), UPDATE_OK);
    CHECK_EQ(send(0, UPDATE_OP_INFO, NULL, 0), UPDATE_OK);
    wait_ms(90);
    CHECK_EQ(commit_runs, 0);
    CHECK_EQ(led_now, UPDATE_LED_WRITING);
    wait_ms(15);
    CHECK_EQ(commit_runs, 1);
    CHECK_EQ(commit_crc, body_crc());
    CHECK_EQ(commit_len, img_len);
    NOTE();
    CHECK_EQ(updater_state(), UPDATE_STATE_ERROR);
    CHECK_EQ(send(CLIENT_B, UPDATE_OP_STATUS, NULL, 0), UPDATE_OK);
    CHECK_EQ(rsp[12], UPDATE_BUSY);
    pass();
    CHECK_EQ(led_now, UPDATE_LED_ERROR);
#undef NOTE
    int led_shown = led_seen;

    const update_state_t want[] = {
        UPDATE_STATE_IDLE,      UPDATE_STATE_MANIFEST_LOADING, UPDATE_STATE_CONFIRM_WAIT, UPDATE_STATE_VERIFYING_MANIFEST, UPDATE_STATE_ERASING,
        UPDATE_STATE_RECEIVING, UPDATE_STATE_VERIFYING_IMAGE,  UPDATE_STATE_VERIFIED,     UPDATE_STATE_COMMITTING,         UPDATE_STATE_ERROR,
    };
    CHECK_EQ(nseen, (int)(sizeof(want) / sizeof(want[0])));
    for (int i = 0; i < nseen && i < (int)(sizeof(want) / sizeof(want[0])); i++) CHECK_EQ(seen[i], want[i]);
    CHECK_EQ(led_shown, (1 << UPDATE_LED_CONFIRM) | (1 << UPDATE_LED_CHORD) | (1 << UPDATE_LED_APPROVED) | (1 << UPDATE_LED_PROGRESS) | (1 << UPDATE_LED_WRITING) | (1 << UPDATE_LED_ERROR));

    // the 50 ms budget, in simulated flash time
    printf("longest op %llu us, longest pass %llu us (signature pass not timed on the host)\n", (unsigned long long)max_op_us, (unsigned long long)max_pass_us);
    CHECK(max_op_us + max_pass_us <= 50000);
    CHECK(max_op_us <= SIM_PAGE_PROGRAM_US); // at most one page per CHUNK

    // ABORT from anyone clears the error, and the LEDs go back
    CHECK_EQ(send_nonce(CLIENT_C, UPDATE_OP_ABORT, 0), UPDATE_OK);
    CHECK_EQ(updater_state(), UPDATE_STATE_IDLE);
    pass();
    CHECK_EQ(led_now, UPDATE_LED_NONE);
}

static void test_session_gesture(void) {
    fresh(0x3000);
    // not watching: keys pass through
    CHECK_EQ(scan(true, true), 3);
    CHECK_EQ(scan(false, false), 0);

    // keys held when ARM arrives are swallowed (QMK sees them released) and do
    // not confirm until let go and pressed again
    scan(true, true);
    CHECK(to_confirm_wait(CLIENT_A));
    CHECK(!hold(SVAL_UPDATE_CHORD_HOLD_MS + 200));
    CHECK(!update_gesture_done(NULL));
    // one key alone, or a hold that is too short, does nothing; all swallowed
    scan(false, false);
    for (int i = 0; i < 150; i++) {
        CHECK_EQ(scan(true, false), 0);
        wait_ms(10);
    }
    scan(false, false);
    CHECK(!hold(SVAL_UPDATE_CHORD_HOLD_MS - 30));
    CHECK(!update_gesture_done(NULL));
    // a bounce restarts the hold
    scan(false, false);
    CHECK(!hold(700));
    CHECK_EQ(scan(true, false), 0);
    CHECK(!hold(700));
    CHECK(!update_gesture_done(NULL));
    CHECK(!hold(400));
    CHECK(update_gesture_done(NULL));
    // still held after BEGIN: swallowed until each key is released
    CHECK_EQ(send_nonce(CLIENT_A, UPDATE_OP_BEGIN, session_nonce), UPDATE_ACCEPTED);
    CHECK_EQ(scan(true, true), 0);
    CHECK_EQ(scan(false, true), 0);
    CHECK_EQ(scan(true, true), 1); // A was released, so it is a new press
    CHECK_EQ(scan(true, false), 1);
    CHECK_EQ(scan(true, true), 3);

    // the chord window: 30 s from ARM, then NOT_CONFIRMED (latched)
    fresh(0x3000);
    CHECK(to_confirm_wait(CLIENT_A));
    for (int i = 0; i < 29; i++) {
        wait_ms(1000);
        CHECK_EQ(send(CLIENT_A, UPDATE_OP_STATUS, NULL, 0), UPDATE_OK); // keeps the session alive
    }
    wait_ms(990);
    CHECK_EQ(updater_state(), UPDATE_STATE_CONFIRM_WAIT);
    CHECK_EQ(scan(true, true), 0);
    wait_ms(20);
    CHECK_EQ(updater_state(), UPDATE_STATE_ERROR);
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_STATUS, NULL, 0), UPDATE_OK);
    CHECK_EQ(rsp[12], UPDATE_NOT_CONFIRMED);
    CHECK_EQ(send_nonce(CLIENT_A, UPDATE_OP_BEGIN, session_nonce), UPDATE_INVALID);
    CHECK_EQ(scan(true, true), 0); // still held: still swallowed
    CHECK_EQ(scan(false, false), 0);
    CHECK_EQ(scan(true, true), 3);
    CHECK_EQ(erase_ops + program_ops, 0);
}

static void test_session_rebind(void) {
    fresh(0x3000);
    CHECK_EQ(send_nonce(CLIENT_A, UPDATE_OP_REBIND, 1), UPDATE_INVALID); // no session
    CHECK_EQ(load_manifest(CLIENT_A), UPDATE_OK);
    CHECK_EQ(send_nonce(CLIENT_A, UPDATE_OP_REBIND, 0), UPDATE_INVALID); // not bound until ARM
    fresh(0x3000);
    CHECK(to_receiving(CLIENT_A));
    CHECK_EQ(send_image(CLIENT_A, 0, 0x1000), UPDATE_OK);
    // wrong nonce: OTHER_CLIENT, nothing moves
    CHECK_EQ(send_nonce(CLIENT_B, UPDATE_OP_REBIND, session_nonce ^ 1), UPDATE_OTHER_CLIENT);
    CHECK_EQ(send_nonce(CLIENT_A, UPDATE_OP_REBIND, session_nonce + 1), UPDATE_OTHER_CLIENT);
    CHECK_EQ(send_image(CLIENT_A, 0x1000, 0x1012), UPDATE_OK);
    CHECK_EQ(send_image(CLIENT_B, 0x1012, 0x1024), UPDATE_OTHER_CLIENT);
    // right nonce: the session moves to B; A is refused from now on
    CHECK_EQ(send_nonce(CLIENT_B, UPDATE_OP_REBIND, session_nonce), UPDATE_OK);
    CHECK_EQ(rsp[1], UPDATE_STATE_RECEIVING);
    CHECK_EQ(send_image(CLIENT_A, 0x1012, 0x1024), UPDATE_OTHER_CLIENT);
    CHECK_EQ(send_chunk(CLIENT_A, 0, 18, img), UPDATE_OTHER_CLIENT); // even a repeat
    CHECK_EQ(send_nonce(CLIENT_A, UPDATE_OP_ABORT, session_nonce), UPDATE_OTHER_CLIENT);
    CHECK_EQ(send_nonce(CLIENT_A, UPDATE_OP_END, session_nonce), UPDATE_OTHER_CLIENT);
    CHECK_EQ(send_image(CLIENT_B, 0x1012, img_len), UPDATE_OK);
    // and again, B to C, mid-way through the session
    CHECK_EQ(send_nonce(CLIENT_C, UPDATE_OP_REBIND, session_nonce), UPDATE_OK);
    CHECK_EQ(send_nonce(CLIENT_B, UPDATE_OP_END, session_nonce), UPDATE_OTHER_CLIENT);
    CHECK_EQ(send_nonce(CLIENT_C, UPDATE_OP_END, session_nonce), UPDATE_ACCEPTED);
    passes_until(UPDATE_STATE_VERIFIED, 2000);
    CHECK_EQ(updater_state(), UPDATE_STATE_VERIFIED);
    // REBIND to the same client is harmless
    CHECK_EQ(send_nonce(CLIENT_C, UPDATE_OP_REBIND, session_nonce), UPDATE_OK);
    CHECK_EQ(updater_state(), UPDATE_STATE_VERIFIED);
    // REBIND keeps a session alive across a 50 s renewal cycle (D3)
    fresh(0x3000);
    CHECK(to_receiving(CLIENT_A));
    uint32_t clients[] = {CLIENT_A, CLIENT_B, CLIENT_C, CLIENT_A + 7};
    uint32_t off       = 0;
    for (int round = 1; round < 4; round++) {
        for (int i = 0; i < 20; i++) {
            wait_ms(2500); // 50 s of slow transfer per round
            uint8_t n = 18;
            CHECK_EQ(send_chunk(clients[round - 1], off, n, img + off), UPDATE_OK);
            off += n;
        }
        CHECK_EQ(send_nonce(clients[round], UPDATE_OP_REBIND, session_nonce), UPDATE_OK);
        CHECK_EQ(send_chunk(clients[round - 1], off, 18, img + off), UPDATE_OTHER_CLIENT);
    }
    CHECK_EQ(updater_state(), UPDATE_STATE_RECEIVING);
}

static void test_session_clients_and_abort(void) {
    // a second client while one loads the manifest
    fresh(0x3000);
    uint8_t a[22] = {0, 20};
    memcpy(a + 2, blob, 20);
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_MANIFEST, a, sizeof(a)), UPDATE_OK);
    CHECK_EQ(send(CLIENT_B, UPDATE_OP_MANIFEST, a, sizeof(a)), UPDATE_OTHER_CLIENT);
    CHECK_EQ(load_manifest(CLIENT_A), UPDATE_OK);
    CHECK_EQ(send(CLIENT_B, UPDATE_OP_ARM, NULL, 0), UPDATE_OTHER_CLIENT);
    CHECK_EQ(send_nonce(CLIENT_B, UPDATE_OP_ABORT, 0), UPDATE_OTHER_CLIENT);
    CHECK_EQ(updater_state(), UPDATE_STATE_MANIFEST_LOADING);
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_ARM, NULL, 0), UPDATE_OK);
    session_nonce = get32le(rsp + 1);
    // then while A's session runs: B is refused everywhere, with no effect
    CHECK_EQ(send(CLIENT_B, UPDATE_OP_MANIFEST, a, sizeof(a)), UPDATE_BUSY);
    CHECK_EQ(send(CLIENT_B, UPDATE_OP_ARM, NULL, 0), UPDATE_BUSY);
    CHECK_EQ(send_nonce(CLIENT_B, UPDATE_OP_BEGIN, session_nonce), UPDATE_OTHER_CLIENT);
    CHECK_EQ(send_nonce(CLIENT_B, UPDATE_OP_ABORT, session_nonce), UPDATE_OTHER_CLIENT);
    CHECK_EQ(send_nonce(CLIENT_A, UPDATE_OP_ABORT, session_nonce ^ 0x100), UPDATE_OTHER_CLIENT); // the session, wrong nonce
    chord();
    CHECK_EQ(send_nonce(CLIENT_B, UPDATE_OP_BEGIN, session_nonce), UPDATE_OTHER_CLIENT);
    CHECK_EQ(send_nonce(CLIENT_A, UPDATE_OP_BEGIN, session_nonce), UPDATE_ACCEPTED);
    passes_until(UPDATE_STATE_RECEIVING, 2000);
    CHECK_EQ(send_chunk(CLIENT_B, 0, 18, img), UPDATE_OTHER_CLIENT);
    CHECK_EQ(send_nonce(CLIENT_B, UPDATE_OP_END, session_nonce), UPDATE_OTHER_CLIENT);
    CHECK_EQ(send(CLIENT_B, UPDATE_OP_STATUS, NULL, 0), UPDATE_OK); // anyone may read STATUS
    CHECK_EQ(rsp[1], UPDATE_STATE_RECEIVING);
    CHECK_EQ(send_image(CLIENT_A, 0, img_len), UPDATE_OK);
    CHECK_EQ(send_nonce(CLIENT_A, UPDATE_OP_END, session_nonce), UPDATE_ACCEPTED);
    passes_until(UPDATE_STATE_VERIFIED, 2000);
    uint8_t c[6];
    put32(c, session_nonce);
    c[4] = body_crc() & 0xFF;
    c[5] = (body_crc() >> 8) & 0xFF;
    commit_avail = true;
    CHECK_EQ(send(CLIENT_B, UPDATE_OP_COMMIT, c, 6), UPDATE_OTHER_CLIENT);
    // a COMMIT for another image (wrong CRC) is refused and the session stays verified
    c[4] ^= 1;
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_COMMIT, c, 6), UPDATE_BAD_HASH);
    put32(c, session_nonce + 1);
    c[4] ^= 1;
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_COMMIT, c, 6), UPDATE_OTHER_CLIENT);
    CHECK_EQ(updater_state(), UPDATE_STATE_VERIFIED);
    CHECK_EQ(commit_runs, 0);

    // ABORT: from the session (client and nonce), in each state; never a flash op
    const update_state_t stops[] = {UPDATE_STATE_MANIFEST_LOADING, UPDATE_STATE_CONFIRM_WAIT, UPDATE_STATE_ERASING, UPDATE_STATE_RECEIVING, UPDATE_STATE_VERIFIED};
    for (size_t i = 0; i < sizeof(stops) / sizeof(stops[0]); i++) {
        fresh(0x3000);
        memset(update_host_flash + SVAL_UPDATE_BASE, 0x00, 0x3000);
        switch (stops[i]) {
            case UPDATE_STATE_MANIFEST_LOADING:
                CHECK_EQ(load_manifest(CLIENT_A), UPDATE_OK);
                break;
            case UPDATE_STATE_CONFIRM_WAIT:
                CHECK(to_confirm_wait(CLIENT_A));
                break;
            case UPDATE_STATE_ERASING:
                CHECK(to_confirm_wait(CLIENT_A));
                chord();
                CHECK_EQ(send_nonce(CLIENT_A, UPDATE_OP_BEGIN, session_nonce), UPDATE_ACCEPTED);
                pass();
                pass(); // signature, then the first sector
                break;
            case UPDATE_STATE_RECEIVING:
                CHECK(to_receiving(CLIENT_A));
                CHECK_EQ(send_image(CLIENT_A, 0, 0x180), UPDATE_OK);
                break;
            default:
                CHECK(to_verified(CLIENT_A));
                break;
        }
        CHECK_EQ(updater_state(), stops[i]);
        int ops = erase_ops + program_ops;
        snap();
        CHECK_EQ(send_nonce(CLIENT_B, UPDATE_OP_ABORT, session_nonce), UPDATE_OTHER_CLIENT);
        CHECK_EQ(updater_state(), stops[i]);
        CHECK_EQ(send_nonce(CLIENT_A, UPDATE_OP_ABORT, session_nonce), UPDATE_OK);
        CHECK_EQ(rsp[1], UPDATE_STATE_IDLE);
        CHECK_EQ(updater_state(), UPDATE_STATE_IDLE);
        wait_ms(5);
        CHECK_EQ(erase_ops + program_ops, ops); // ABORT does not erase
        CHECK(memcmp(before, update_host_flash, sizeof(before)) == 0);
        CHECK_EQ(led_now, UPDATE_LED_NONE);
    }
    // in IDLE: nothing to do
    CHECK_EQ(send_nonce(CLIENT_B, UPDATE_OP_ABORT, 0), UPDATE_OK);
    // ERROR: anyone wrapped may clear it, but not without the wrapper
    fresh(0x3000);
    CHECK(to_receiving(CLIENT_A));
    CHECK_EQ(send_chunk(CLIENT_A, img_len - 10, 18, img), UPDATE_OUT_OF_ORDER);
    CHECK_EQ(send_image(CLIENT_A, 0, img_len), UPDATE_OK);
    CHECK_EQ(send_chunk(CLIENT_A, img_len, 1, img), UPDATE_OVERRUN);
    CHECK_EQ(updater_state(), UPDATE_STATE_ERROR);
    CHECK_EQ(send_nonce(0, UPDATE_OP_ABORT, 0), UPDATE_INVALID);
    CHECK_EQ(updater_state(), UPDATE_STATE_ERROR);
    CHECK_EQ(send(CLIENT_B, UPDATE_OP_MANIFEST, a, sizeof(a)), UPDATE_BUSY); // ABORT first
    CHECK_EQ(send_nonce(CLIENT_B, UPDATE_OP_ABORT, 0), UPDATE_OK);
    CHECK_EQ(updater_state(), UPDATE_STATE_IDLE);
    // and a new session can start straight away, from anyone
    CHECK(to_confirm_wait(CLIENT_B));
}

static void test_session_chunks(void) {
    fresh(0x3000);
    CHECK(to_receiving(CLIENT_A));
    int ops = program_ops;
    // lengths
    CHECK_EQ(send_chunk(CLIENT_A, 0, 0, img), UPDATE_INVALID);
    CHECK_EQ(send_chunk(CLIENT_A, 0, 19, img), UPDATE_INVALID);
    // out of order: ahead of the next offset, refused with the offset expected
    CHECK_EQ(send_chunk(CLIENT_A, 18, 18, img + 18), UPDATE_OUT_OF_ORDER);
    CHECK_EQ(get24le(rsp + 1), 0);
    CHECK_EQ(send_chunk(CLIENT_A, 0xFFFFFF, 18, img), UPDATE_OUT_OF_ORDER);
    CHECK_EQ(updater_state(), UPDATE_STATE_RECEIVING);
    // in order across a page boundary: one page programmed when it fills
    CHECK_EQ(send_image(CLIENT_A, 0, 252), UPDATE_OK);
    CHECK_EQ(program_ops, ops);
    CHECK_EQ(send_chunk(CLIENT_A, 252, 18, img + 252), UPDATE_OK);
    CHECK_EQ(get24le(rsp + 1), 270);
    CHECK_EQ(program_ops, ops + 1);
    CHECK(memcmp(update_host_flash + SVAL_UPDATE_BASE, img, 256) == 0);
    // a repeat of an earlier chunk is acknowledged, not rewritten, even with other bytes
    uint8_t junk[18];
    memset(junk, 0x00, sizeof(junk));
    CHECK_EQ(send_chunk(CLIENT_A, 0, 18, junk), UPDATE_OK);
    CHECK_EQ(get24le(rsp + 1), 270);
    CHECK_EQ(send_chunk(CLIENT_A, 252, 18, img + 252), UPDATE_OK);
    CHECK_EQ(send_chunk(CLIENT_A, 260, 10, img + 260), UPDATE_OK);
    CHECK_EQ(program_ops, ops + 1);
    CHECK(memcmp(update_host_flash + SVAL_UPDATE_BASE, img, 256) == 0);
    // overlapping the next offset is not a repeat
    CHECK_EQ(send_chunk(CLIENT_A, 260, 18, img + 260), UPDATE_OUT_OF_ORDER);
    // END too early
    CHECK_EQ(send_nonce(CLIENT_A, UPDATE_OP_END, session_nonce), UPDATE_OUT_OF_ORDER);
    CHECK_EQ(updater_state(), UPDATE_STATE_RECEIVING);
    // the rest; the last chunk ends exactly at image_len
    CHECK_EQ(send_image(CLIENT_A, 270, img_len), UPDATE_OK);
    CHECK_EQ(program_ops, ops + (int)(img_len / 256));
    // overlapping the end of what was staged is not a repeat either
    CHECK_EQ(send_chunk(CLIENT_A, img_len - 4, 18, img), UPDATE_OUT_OF_ORDER);
    CHECK_EQ(updater_state(), UPDATE_STATE_RECEIVING);
    // a chunk past the end: OVERRUN, latched
    fresh(0x3000);
    CHECK(to_receiving(CLIENT_A));
    CHECK_EQ(send_image(CLIENT_A, 0, img_len - 10), UPDATE_OK);
    CHECK_EQ(send_chunk(CLIENT_A, img_len - 10, 18, img + img_len - 10), UPDATE_OVERRUN);
    CHECK_EQ(updater_state(), UPDATE_STATE_ERROR);
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_STATUS, NULL, 0), UPDATE_OK);
    CHECK_EQ(rsp[12], UPDATE_OVERRUN);
    pass();
    CHECK_EQ(led_now, UPDATE_LED_ERROR);
    CHECK_EQ(send_chunk(CLIENT_A, img_len - 10, 10, img + img_len - 10), UPDATE_INVALID); // nothing more after ERROR
    // CHUNK and END out of state
    fresh(0x3000);
    CHECK_EQ(send_chunk(CLIENT_A, 0, 18, img), UPDATE_INVALID);
    CHECK(to_confirm_wait(CLIENT_A));
    CHECK_EQ(send_chunk(CLIENT_A, 0, 18, img), UPDATE_INVALID);
    CHECK_EQ(send_nonce(CLIENT_A, UPDATE_OP_END, session_nonce), UPDATE_INVALID);
    // a flash fault while programming: FLASH_ERR, latched
    fresh(0x3000);
    CHECK(to_receiving(CLIENT_A));
    tear_next = 1;
    CHECK_EQ(send_image(CLIENT_A, 0, 256), UPDATE_FLASH_ERR);
    CHECK_EQ(updater_state(), UPDATE_STATE_ERROR);
}

static void test_session_timeouts(void) {
    // no op from the session for 30 s: TIMEOUT, latched, in every waiting state
    const update_state_t stops[] = {UPDATE_STATE_CONFIRM_WAIT, UPDATE_STATE_RECEIVING, UPDATE_STATE_VERIFIED};
    for (size_t i = 0; i < sizeof(stops) / sizeof(stops[0]); i++) {
        fresh(0x3000);
        if (stops[i] == UPDATE_STATE_CONFIRM_WAIT) {
            CHECK(to_confirm_wait(CLIENT_A));
            chord(); // chord made, but no BEGIN
        } else if (stops[i] == UPDATE_STATE_RECEIVING) {
            CHECK(to_receiving(CLIENT_A));
        } else {
            CHECK(to_verified(CLIENT_A));
        }
        // another client's STATUS does not keep it alive
        CHECK_EQ(send(CLIENT_A, UPDATE_OP_STATUS, NULL, 0), UPDATE_OK); // the session's does
        for (int k = 0; k < 29; k++) {
            wait_ms(1000);
            CHECK_EQ(send(CLIENT_B, UPDATE_OP_STATUS, NULL, 0), UPDATE_OK);
        }
        CHECK_EQ(updater_state(), stops[i]);
        wait_ms(1100);
        CHECK_EQ(updater_state(), UPDATE_STATE_ERROR);
        CHECK_EQ(send(CLIENT_A, UPDATE_OP_STATUS, NULL, 0), UPDATE_OK);
        CHECK_EQ(rsp[12], UPDATE_TIMEOUT);
        CHECK_EQ(send_nonce(CLIENT_A, UPDATE_OP_ABORT, session_nonce), UPDATE_OK);
    }
    // the session's own ops keep it alive: a slow transfer, 25 s between chunks
    fresh(0x3000);
    CHECK(to_receiving(CLIENT_A));
    for (uint32_t off = 0; off < 18 * 4; off += 18) {
        wait_ms(25000);
        CHECK_EQ(send_chunk(CLIENT_A, off, 18, img + off), UPDATE_OK);
    }
    CHECK_EQ(updater_state(), UPDATE_STATE_RECEIVING);
    // a half-sent manifest is dropped quietly after 30 s (it was never shown)
    fresh(0x3000);
    uint8_t a[22] = {0, 20};
    memcpy(a + 2, blob, 20);
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_MANIFEST, a, sizeof(a)), UPDATE_OK);
    wait_ms(SVAL_UPDATE_SESSION_TIMEOUT_MS + 10);
    CHECK_EQ(updater_state(), UPDATE_STATE_IDLE);
    CHECK_EQ(send(CLIENT_B, UPDATE_OP_MANIFEST, a, sizeof(a)), UPDATE_OK);
    // COMMITTING: no session timeout and no ABORT (D23); the commit runs 100 ms
    // after the reply, and a refusal it returns is latched
    fresh(0x3000);
    CHECK(to_verified(CLIENT_A));
    commit_avail  = true;
    commit_result = UPDATE_FLASH_ERR;
    uint8_t c[6];
    put32(c, session_nonce);
    c[4] = body_crc() & 0xFF;
    c[5] = (body_crc() >> 8) & 0xFF;
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_COMMIT, c, 6), UPDATE_ACCEPTED);
    pass();
    CHECK_EQ(updater_state(), UPDATE_STATE_COMMITTING);
    wait_ms(200);
    CHECK_EQ(commit_runs, 1);
    CHECK_EQ(updater_state(), UPDATE_STATE_ERROR);
}

static update_status_t refusal_after_begin(void) {
    if (!to_confirm_wait(CLIENT_A)) return (update_status_t)rsp[0];
    chord();
    if (send_nonce(CLIENT_A, UPDATE_OP_BEGIN, session_nonce) != UPDATE_ACCEPTED) return (update_status_t)rsp[0];
    passes_until(UPDATE_STATE_RECEIVING, 2000);
    send(CLIENT_A, UPDATE_OP_STATUS, NULL, 0);
    return updater_state() == UPDATE_STATE_ERROR ? (update_status_t)rsp[12] : UPDATE_OK;
}

static void mutate_and_sign(void (*f)(sval_update_manifest_t *)) {
    f(blob_manifest());
    sign_blob();
}

static void m_hand(sval_update_manifest_t *m) { m->hand = UPDATE_HAND_LEFT; }
static void m_pointing(sval_update_manifest_t *m) { m->pointing_id = UPDATE_POINTING_PMW3360; }
static void m_large(sval_update_manifest_t *m) { m->image_len = SVAL_UPDATE_MAX_IMAGE + 0x100; }
static void m_unaligned(sval_update_manifest_t *m) { m->image_len = 0x3010; }
static void m_storage(sval_update_manifest_t *m) { m->storage_format = SVAL_UPDATE_STORAGE_FORMAT - 1; }
static void m_magic(sval_update_manifest_t *m) { m->magic ^= 0x100; }
static void m_release(sval_update_manifest_t *m) { m->flags = UPDATE_FLAG_RELEASE; }
static void m_key(sval_update_manifest_t *m) { m->key_id = UPDATE_KEY_RELEASE_1; }
static void m_hash(sval_update_manifest_t *m) { m->sha512[17] ^= 0x20; }

static void test_session_refusals(void) {
    // refused at ARM, latched: wrong hand, variant, length, floors, magic, key
    struct {
        void (*mutate)(sval_update_manifest_t *);
        update_status_t want;
    } const arm_cases[] = {
        {m_hand, UPDATE_UNSUPPORTED}, {m_pointing, UPDATE_WRONG_HW}, {m_large, UPDATE_TOO_LARGE}, {m_unaligned, UPDATE_BAD_IMAGE},
        {m_storage, UPDATE_EPOCH},    {m_magic, UPDATE_INVALID},     {m_release, UPDATE_BAD_SIG},
    };
    for (size_t i = 0; i < sizeof(arm_cases) / sizeof(arm_cases[0]); i++) {
        fresh(0x3000);
        mutate_and_sign(arm_cases[i].mutate);
        CHECK_EQ(load_manifest(CLIENT_A), UPDATE_OK);
        CHECK_EQ(send(CLIENT_A, UPDATE_OP_ARM, NULL, 0), arm_cases[i].want);
        CHECK_EQ(get32le(rsp + 1), 0); // no nonce
        CHECK_EQ(updater_state(), UPDATE_STATE_ERROR);
        pass();
        CHECK_EQ(led_now, UPDATE_LED_ERROR);
        CHECK_EQ(erase_ops + program_ops, 0);
    }
    // the security epoch floor
    fresh(0x3000);
    self_dev.security_epoch = 1;
    CHECK_EQ(load_manifest(CLIENT_A), UPDATE_OK);
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_ARM, NULL, 0), UPDATE_EPOCH);
    // a release build refuses the test key
    fresh(0x3000);
    self_dev.release_build = true;
    CHECK_EQ(load_manifest(CLIENT_A), UPDATE_OK);
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_ARM, NULL, 0), UPDATE_BAD_SIG);

    // refused after BEGIN (the signature), before any erase
    fresh(0x3000);
    blob[UPDATE_MANIFEST_BYTES + 5] ^= 0x01; // a flipped signature byte
    CHECK_EQ(refusal_after_begin(), UPDATE_BAD_SIG);
    CHECK_EQ(erase_ops, 0);
    fresh(0x3000);
    mutate_and_sign(m_key); // key_id 1: no release key in this build
    CHECK_EQ(refusal_after_begin(), UPDATE_BAD_SIG);
    fresh(0x3000);
    blob[30] ^= 0x01; // the version string, after signing
    CHECK_EQ(refusal_after_begin(), UPDATE_BAD_SIG);
    CHECK_EQ(erase_ops, 0);

    // refused at VERIFYING_IMAGE: the bytes are not the signed ones, or the
    // signed image is structurally bad
    struct {
        uint32_t        at;
        uint32_t        value;
        update_status_t want;
    } const image_cases[] = {
        {0x40, 0x12345678, UPDATE_BAD_HASH},  // a byte differs from the signed hash (signed over the original)
        {0x10, 0, UPDATE_BAD_IMAGE},          // boot2 CRC
        {0x100, 0x1FFFFFF0, UPDATE_BAD_IMAGE}, // initial SP
        {0x104, 0x100001F6, UPDATE_BAD_IMAGE}, // reset vector even
        {0x104, 0x10003001, UPDATE_BAD_IMAGE}, // reset vector past the image
    };
    for (size_t i = 0; i < sizeof(image_cases) / sizeof(image_cases[0]); i++) {
        fresh(0x3000);
        put32(img + image_cases[i].at, image_cases[i].value);
        if (image_cases[i].want == UPDATE_BAD_IMAGE) {
            crypto_sha512(blob_manifest()->sha512, img, img_len); // signed as it is
            sign_blob();
        }
        CHECK(to_receiving(CLIENT_A));
        CHECK_EQ(send_image(CLIENT_A, 0, img_len), UPDATE_OK);
        CHECK_EQ(send_nonce(CLIENT_A, UPDATE_OP_END, session_nonce), UPDATE_ACCEPTED);
        passes_until(UPDATE_STATE_VERIFIED, 2000);
        CHECK_EQ(updater_state(), UPDATE_STATE_ERROR);
        CHECK_EQ(send(CLIENT_A, UPDATE_OP_STATUS, NULL, 0), UPDATE_OK);
        CHECK_EQ(rsp[12], image_cases[i].want);
        CHECK_EQ(get16le(rsp + 19), 0); // no CRC offered for COMMIT
    }
    fresh(0x3000);
    mutate_and_sign(m_hash);
    CHECK(to_receiving(CLIENT_A));
    CHECK_EQ(send_image(CLIENT_A, 0, img_len), UPDATE_OK);
    CHECK_EQ(send_nonce(CLIENT_A, UPDATE_OP_END, session_nonce), UPDATE_ACCEPTED);
    passes_until(UPDATE_STATE_VERIFIED, 2000);
    CHECK_EQ(updater_state(), UPDATE_STATE_ERROR);

    // settings writes failing (D12): ARM, BEGIN and COMMIT refuse, with no state change
    fresh(0x3000);
    CHECK_EQ(load_manifest(CLIENT_A), UPDATE_OK);
    port_failing = true;
    CHECK_EQ(send(0, UPDATE_OP_INFO, NULL, 0), UPDATE_UNAVAILABLE);
    CHECK_EQ(rsp[21] & 0x02, 0x02);
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_ARM, NULL, 0), UPDATE_UNAVAILABLE);
    CHECK_EQ(updater_state(), UPDATE_STATE_MANIFEST_LOADING);
    port_failing = false;
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_ARM, NULL, 0), UPDATE_OK);
    session_nonce = get32le(rsp + 1);
    chord();
    port_failing = true;
    CHECK_EQ(send_nonce(CLIENT_A, UPDATE_OP_BEGIN, session_nonce), UPDATE_UNAVAILABLE);
    CHECK_EQ(updater_state(), UPDATE_STATE_CONFIRM_WAIT);
    port_failing = false;
    CHECK_EQ(send_nonce(CLIENT_A, UPDATE_OP_BEGIN, session_nonce), UPDATE_ACCEPTED);
    passes_until(UPDATE_STATE_RECEIVING, 2000);
    CHECK_EQ(send_image(CLIENT_A, 0, img_len), UPDATE_OK);
    CHECK_EQ(send_nonce(CLIENT_A, UPDATE_OP_END, session_nonce), UPDATE_ACCEPTED);
    passes_until(UPDATE_STATE_VERIFIED, 2000);
    uint8_t c[6];
    put32(c, session_nonce);
    c[4]         = body_crc() & 0xFF;
    c[5]         = (body_crc() >> 8) & 0xFF;
    commit_avail = true;
    port_failing = true;
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_COMMIT, c, 6), UPDATE_UNAVAILABLE);
    CHECK_EQ(updater_state(), UPDATE_STATE_VERIFIED);
    port_failing = false;
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_COMMIT, c, 6), UPDATE_ACCEPTED);

    // a die that is not 16 MiB (D26): unavailable, nothing touched
    fresh(0x3000);
    capacity = 0x15;
    snap();
    CHECK_EQ(send(0, UPDATE_OP_INFO, NULL, 0), UPDATE_UNAVAILABLE);
    CHECK_EQ(rsp[21] & 0x01, 0);
    CHECK_EQ(load_manifest(CLIENT_A), UPDATE_UNAVAILABLE);
    CHECK_EQ(updater_state(), UPDATE_STATE_IDLE);
    CHECK(memcmp(before, update_host_flash, sizeof(before)) == 0);
    capacity = 0x18;

    // MANIFEST rules
    fresh(0x3000);
    uint8_t a[22] = {20, 20};
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_MANIFEST, a, sizeof(a)), UPDATE_OUT_OF_ORDER); // must start at 0
    CHECK_EQ(updater_state(), UPDATE_STATE_IDLE);
    a[0] = 0;
    a[1] = 21;
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_MANIFEST, a, sizeof(a)), UPDATE_INVALID);
    a[1] = 0;
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_MANIFEST, a, sizeof(a)), UPDATE_INVALID);
    a[0] = 160;
    a[1] = 20;
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_MANIFEST, a, sizeof(a)), UPDATE_INVALID); // past 172
    CHECK_EQ(load_manifest(CLIENT_A), UPDATE_OK);                                 // offset 0 restarts
    a[0] = 40;
    a[1] = 4;
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_MANIFEST, a, sizeof(a)), UPDATE_OUT_OF_ORDER);
    CHECK_EQ(rsp[1], UPDATE_SIGNED_MANIFEST_BYTES);
    // ARM before the whole manifest
    fresh(0x3000);
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_ARM, NULL, 0), UPDATE_INVALID);
    a[0] = 0;
    a[1] = 20;
    memcpy(a + 2, blob, 20);
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_MANIFEST, a, sizeof(a)), UPDATE_OK);
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_ARM, NULL, 0), UPDATE_INVALID);
    CHECK_EQ(updater_state(), UPDATE_STATE_MANIFEST_LOADING);
    // ARM twice: the second is BUSY and the nonce stays
    CHECK(to_confirm_wait(CLIENT_A));
    uint32_t first = session_nonce;
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_ARM, NULL, 0), UPDATE_BUSY);
    chord();
    CHECK_EQ(send_nonce(CLIENT_A, UPDATE_OP_BEGIN, first), UPDATE_ACCEPTED);
    CHECK_EQ(send_nonce(CLIENT_A, UPDATE_OP_BEGIN, first), UPDATE_INVALID); // once
}

static void test_session(void) {
    uint8_t seed[32], pk[32];
    hex(test_seed_hex, seed, 32);
    crypto_ed25519_key_pair(sk, pk, seed);
    CHECK(memcmp(pk, update_key(UPDATE_KEY_TEST), 32) == 0); // the seed signs for the embedded test key
    int before_failures = failures;
    test_session_info_and_wrapper();
    test_session_happy_path();
    test_session_gesture();
    test_session_rebind();
    test_session_clients_and_abort();
    test_session_chunks();
    test_session_timeouts();
    test_session_refusals();
    printf("session tests: %s\n", failures == before_failures ? "pass" : "FAIL");
}
