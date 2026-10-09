// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

// Host tests for the updater's split link (M2a stage 1): the split pause
// (keyboards/svalboard/split_pause.c, D15), the KEYBOARD_UPDATE wire format and
// page assembly (update_split_wire.c), presence with version and the mismatch
// rules (D17, V), and the slave's request handler (update_split.c). Built by
// util/updater_test/run.sh under ASan and UBSan.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "split_pause_host.h"
#include "split_pause.h"
#include "update_split.h"

static int checks, failures;

#define CHECK(cond)                                                                   \
    do {                                                                              \
        checks++;                                                                     \
        if (!(cond)) {                                                                \
            failures++;                                                               \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                             \
    } while (0)

#define CHECK_EQ(a, b)                                                                                                       \
    do {                                                                                                                     \
        long long _a = (long long)(a), _b = (long long)(b);                                                                  \
        checks++;                                                                                                            \
        if (_a != _b) {                                                                                                      \
            failures++;                                                                                                      \
            fprintf(stderr, "%s:%d: CHECK_EQ failed: %s == %lld, expected %s == %lld\n", __FILE__, __LINE__, #a, _a, #b, _b); \
        }                                                                                                                    \
    } while (0)

// ---- QMK mocks for split_pause.c ----------------------------------------------------------
// The keyboard as core runs it: raw_matrix and matrix as quantum/matrix_common.c
// defines them, this half on the right (rows 5-9, master), the other half on
// rows 0-4.

matrix_row_t raw_matrix[MATRIX_ROWS];
matrix_row_t matrix[MATRIX_ROWS];
uint8_t      thisHand, thatHand;

static matrix_row_t   local_rows[MATRIX_ROWS_PER_HAND]; // keys held on this half
static matrix_row_t   other_rows[MATRIX_ROWS_PER_HAND]; // keys held on the other half
static bool           master = true, link_up = true;
static int            post_scans, scan_kbs, wd_updates;
static bool           wd_last = true;
static int            shared_sets;
static report_mouse_t shared_report;

bool matrix_scan_custom(matrix_row_t m[]) {
    bool changed = memcmp(m, local_rows, sizeof(local_rows)) != 0;
    memcpy(m, local_rows, sizeof(local_rows));
    return changed;
}

bool debounce(matrix_row_t raw[], matrix_row_t cooked[], bool changed) {
    (void)changed;
    bool c = memcmp(cooked, raw, MATRIX_ROWS_PER_HAND) != 0;
    memcpy(cooked, raw, MATRIX_ROWS_PER_HAND);
    return c;
}

// core matrix_post_scan() on the master: the exchange, then matrix_scan_kb().
bool matrix_post_scan(void) {
    post_scans++;
    bool changed = false;
    if (link_up) {
        changed = memcmp(matrix + thatHand, other_rows, sizeof(other_rows)) != 0;
        if (changed) memcpy(matrix + thatHand, other_rows, sizeof(other_rows));
        // the split pointing exchange: the other half's report
        pointing_device_set_shared_report((report_mouse_t){.x = 7, .y = -3});
    }
    matrix_scan_kb();
    return changed;
}

void matrix_scan_kb(void) {
    scan_kbs++;
}
bool is_keyboard_master(void) {
    return master;
}
bool is_transport_connected(void) {
    return link_up;
}
void split_watchdog_update(bool done) {
    wd_updates++;
    wd_last = done;
}
void pointing_device_set_shared_report(report_mouse_t report) {
    shared_sets++;
    shared_report = report;
}

// keyboard_task(): on a reported change, compare with the previous matrix and
// count key events.
static matrix_row_t prev[MATRIX_ROWS];
static int          presses, releases;

static bool task(void) {
    bool changed = matrix_scan() != 0;
    if (changed) {
        for (int r = 0; r < MATRIX_ROWS; r++) {
            matrix_row_t diff = matrix[r] ^ prev[r];
            for (int c = 0; c < 8; c++) {
                if (!(diff & (1u << c))) continue;
                if (matrix[r] & (1u << c)) {
                    presses++;
                } else {
                    releases++;
                }
            }
            prev[r] = matrix[r];
        }
    }
    return changed;
}

static void pause_fresh(void) {
    sval_split_pause(false);
    memset(raw_matrix, 0, sizeof(raw_matrix));
    memset(matrix, 0, sizeof(matrix));
    memset(prev, 0, sizeof(prev));
    memset(local_rows, 0, sizeof(local_rows));
    memset(other_rows, 0, sizeof(other_rows));
    thisHand = MATRIX_ROWS_PER_HAND;
    thatHand = 0;
    master = link_up = true;
    post_scans = scan_kbs = wd_updates = shared_sets = presses = releases = 0;
    wd_last = true;
    memset(&shared_report, 0, sizeof(shared_report));
}

static void test_pause(void) {
    pause_fresh();
    CHECK(!sval_split_paused()); // off at boot

    // Only the master pauses, and only with the link up; a refusal changes nothing.
    link_up = false;
    CHECK(!sval_split_pause(true));
    CHECK(!sval_split_paused());
    link_up = true;
    master  = false;
    CHECK(!sval_split_pause(true));
    CHECK(!sval_split_paused());
    master = true;
    CHECK_EQ(wd_updates, 0);

    // Unpaused: exactly core's matrix_scan().
    other_rows[1] = 0x04; // a key held on the other half
    CHECK(task());
    CHECK_EQ(presses, 1);
    CHECK_EQ(post_scans, 1);
    CHECK_EQ(scan_kbs, 1);
    CHECK_EQ(matrix[thatHand + 1], 0x04);
    CHECK(!task());
    CHECK_EQ(post_scans, 2);

    // Pause with that key held: the first paused scan releases it, once.
    int sets = shared_sets;
    CHECK(sval_split_pause(true));
    CHECK(sval_split_paused());
    CHECK_EQ(wd_updates, 0); // pausing does not touch the split watchdog
    CHECK(task());
    CHECK_EQ(releases, 1);
    CHECK_EQ(matrix[thatHand + 1], 0);
    CHECK_EQ(post_scans, 2); // no exchange
    CHECK_EQ(scan_kbs, 3);   // matrix_scan_kb() still runs every scan
    CHECK_EQ(shared_sets, sets + 1); // the other half's pointer motion is dropped
    CHECK(shared_report.x == 0 && shared_report.y == 0 && shared_report.buttons == 0);
    // Later paused scans: no change, nothing from the other half, no exchange.
    other_rows[2] = 0x01;
    CHECK(!task());
    CHECK(!task());
    CHECK_EQ(post_scans, 2);
    CHECK_EQ(scan_kbs, 5);
    CHECK_EQ(shared_sets, sets + 1);
    for (int r = 0; r < MATRIX_ROWS_PER_HAND; r++) CHECK_EQ(matrix[thatHand + r], 0);
    // This half's keys keep working while paused.
    local_rows[3] = 0x10;
    CHECK(task());
    CHECK_EQ(presses, 2);
    CHECK_EQ(matrix[thisHand + 3], 0x10);
    local_rows[3] = 0;
    CHECK(task());
    CHECK_EQ(releases, 2);
    // Pausing again changes nothing (no second release pass).
    CHECK(sval_split_pause(true));
    CHECK(!task());

    // Resume: the split watchdog is re-armed (so a half that rebooted is pinged
    // again), and the next scan exchanges and sees the other half's keys.
    CHECK(sval_split_pause(false));
    CHECK(!sval_split_paused());
    CHECK_EQ(wd_updates, 1);
    CHECK(!wd_last);
    CHECK(task());
    CHECK_EQ(post_scans, 3);
    CHECK_EQ(presses, 4); // rows 1 and 2 of the other half
    CHECK_EQ(matrix[thatHand + 1], 0x04);
    CHECK_EQ(matrix[thatHand + 2], 0x01);
    // Resuming an unpaused link does nothing.
    CHECK(sval_split_pause(false));
    CHECK_EQ(wd_updates, 1);

    // A pause and a resume with no scan between: nothing to release, the
    // watchdog is still re-armed.
    releases = 0;
    CHECK(sval_split_pause(true));
    CHECK(sval_split_pause(false));
    CHECK_EQ(wd_updates, 2);
    CHECK(!task());
    CHECK_EQ(releases, 0);
    CHECK_EQ(matrix[thatHand + 1], 0x04);

    // The link drops while paused: the resume still re-arms; core's own
    // disconnect path takes over from there.
    CHECK(sval_split_pause(true));
    CHECK(task());
    link_up = false;
    CHECK(!task());
    CHECK(sval_split_pause(false));
    CHECK_EQ(wd_updates, 3);
    pause_fresh();
}

// ---- update_split.c port --------------------------------------------------------------

static update_build_id_t my_id = {.fw_version = 2001, .git_hash = 0x3b2ca7e1, .git_dirty = false, .updater_proto = UPDATE_PROTOCOL_VERSION, .hand = UPDATE_HAND_RIGHT, .pointing_id = UPDATE_POINTING_PMW3389};
static bool              port_updating;
static int               port_info_calls;

void update_split_port_build_id(update_build_id_t *id) {
    *id = my_id;
}
uint8_t update_split_port_info_flags(void) {
    port_info_calls++;
    return 0x11;
}
bool update_split_port_updating(void) {
    return port_updating;
}

// ---- checksums and the git hash ---------------------------------------------------------

static void test_checks(void) {
    const uint8_t v[] = "123456789";
    CHECK_EQ(usplit_crc16(v, 9), 0x29B1); // CRC-16/CCITT-FALSE check value
    CHECK_EQ(usplit_crc8(v, 9), 0xF4);    // CRC-8/SMBUS check value
    bool dirty = true;
    CHECK_EQ(usplit_git_hash32("3b2ca7e139", &dirty), 0x3b2ca7e1);
    CHECK(!dirty);
    CHECK_EQ(usplit_git_hash32("3B2CA7E139*", &dirty), 0x3b2ca7e1);
    CHECK(dirty);
    CHECK_EQ(usplit_git_hash32("NA", &dirty), 0); // SKIP_GIT
    CHECK(!dirty);
    CHECK_EQ(usplit_git_hash32("3b2ca7e", &dirty), 0); // too short
    CHECK_EQ(usplit_git_hash32("3b2ca7g139", &dirty), 0);
    CHECK_EQ(usplit_git_hash32(NULL, &dirty), 0);
}

// ---- presence --------------------------------------------------------------------------

static void test_presence(void) {
    update_split_host_reset();
    CHECK_EQ(sizeof(update_presence_t), 17); // sizeof(presence_rpc_t), what the master reads
    update_build_id_t other = my_id;
    other.hand              = UPDATE_HAND_LEFT;
    other.pointing_id       = UPDATE_POINTING_TRACKPOINT; // a different build of the same release (D18)
    update_presence_t p, seen;
    usplit_presence_encode(&p, &other, false);
    const uint8_t *b = (const uint8_t *)&p;
    CHECK(b[0] == 'P' && b[1] == 'V'); // magic, as bytes on the wire
    CHECK_EQ(p.crc8, usplit_crc8(b, 16));
    CHECK_EQ(usplit_presence_compare(&p, sizeof(p), &my_id, &seen), UPDATE_PRESENCE_MATCH);
    CHECK_EQ(seen.pointing_id, UPDATE_POINTING_TRACKPOINT);
    CHECK_EQ(usplit_presence_compare(&p, sizeof(p) - 1, &my_id, NULL), UPDATE_PRESENCE_INVALID); // short
    // Any single changed bit: INVALID (the CRC), never a false match.
    for (size_t i = 0; i < sizeof(p) * 8; i++) {
        update_presence_t q = p;
        ((uint8_t *)&q)[i / 8] ^= (uint8_t)(1u << (i % 8));
        CHECK_EQ(usplit_presence_compare(&q, sizeof(q), &my_id, NULL), UPDATE_PRESENCE_INVALID);
    }
    // Stale bytes from a half that does not fill the response.
    uint8_t zeros[17] = {0}, stale[17];
    memset(stale, 0xA5, sizeof(stale));
    CHECK_EQ(usplit_presence_compare(zeros, sizeof(zeros), &my_id, NULL), UPDATE_PRESENCE_INVALID);
    CHECK_EQ(usplit_presence_compare(stale, sizeof(stale), &my_id, NULL), UPDATE_PRESENCE_INVALID);
    // What makes a mismatch.
    update_build_id_t o = other;
    o.fw_version++;
    usplit_presence_encode(&p, &o, false);
    CHECK_EQ(usplit_presence_compare(&p, sizeof(p), &my_id, NULL), UPDATE_PRESENCE_VERSION);
    o = other;
    o.git_hash ^= 1;
    usplit_presence_encode(&p, &o, false);
    CHECK_EQ(usplit_presence_compare(&p, sizeof(p), &my_id, NULL), UPDATE_PRESENCE_VERSION);
    o = other;
    o.updater_proto++;
    usplit_presence_encode(&p, &o, false);
    CHECK_EQ(usplit_presence_compare(&p, sizeof(p), &my_id, NULL), UPDATE_PRESENCE_VERSION);
    o = other;
    o.hand = my_id.hand;
    usplit_presence_encode(&p, &o, false);
    CHECK_EQ(usplit_presence_compare(&p, sizeof(p), &my_id, NULL), UPDATE_PRESENCE_SAME_HAND);
    // Not a mismatch: dirty tree flag, the other half updating.
    o           = other;
    o.git_dirty = true;
    usplit_presence_encode(&p, &o, true);
    CHECK_EQ(p.flags, UPDATE_PRESENCE_FLAG_DIRTY | UPDATE_PRESENCE_FLAG_UPDATING);
    CHECK_EQ(usplit_presence_compare(&p, sizeof(p), &my_id, NULL), UPDATE_PRESENCE_MATCH);

    // The slave fills the RPC response; the master reads it back.
    uint8_t out[32];
    memset(out, 0xEE, sizeof(out));
    update_split_presence_fill(out, 16); // too short: untouched
    for (int i = 0; i < 32; i++) CHECK_EQ(out[i], 0xEE);
    port_updating = true;
    update_split_presence_fill(out, 17);
    port_updating = false;
    for (int i = 17; i < 32; i++) CHECK_EQ(out[i], 0xEE); // nothing past the response
    memcpy(&p, out, sizeof(p));
    CHECK_EQ(p.flags, UPDATE_PRESENCE_FLAG_UPDATING);
    CHECK_EQ(p.hand, my_id.hand);
    // (a slave built for this same hand: the master sees SAME_HAND)
    CHECK_EQ(update_split_presence(), UPDATE_PRESENCE_NONE);
    CHECK(!update_split_mismatch());
    update_split_presence_result(true, out, 17);
    CHECK_EQ(update_split_presence(), UPDATE_PRESENCE_SAME_HAND);
    CHECK(update_split_mismatch());
    usplit_presence_encode(&p, &other, false);
    update_split_presence_result(true, &p, 17);
    CHECK_EQ(update_split_presence(), UPDATE_PRESENCE_MATCH);
    CHECK(!update_split_mismatch());
    update_split_presence_result(true, zeros, 17);
    CHECK_EQ(update_split_presence(), UPDATE_PRESENCE_INVALID);
    CHECK(update_split_mismatch());
    update_split_presence_result(false, NULL, 0); // no answer: nothing to compare, no LED
    CHECK_EQ(update_split_presence(), UPDATE_PRESENCE_NONE);
    CHECK(!update_split_mismatch());
    o = other;
    o.fw_version = 2002;
    usplit_presence_encode(&p, &o, false);
    update_split_presence_result(true, &p, 17);
    CHECK_EQ(update_split_presence(), UPDATE_PRESENCE_VERSION);
    CHECK(update_split_mismatch());
}

// ---- KEYBOARD_UPDATE frames ------------------------------------------------------------

static void test_wire(void) {
    uint8_t      buf[USPLIT_MSG_MAX], data[32];
    usplit_req_t r;
    for (int i = 0; i < 32; i++) data[i] = (uint8_t)(0x30 + i);

    // Response lengths per op; every frame fits the 32-byte RPC buffers.
    CHECK_EQ(usplit_rsp_len(USPLIT_OP_INFO), 4 + 15 + 2);
    CHECK_EQ(usplit_rsp_len(USPLIT_OP_BEGIN), 4 + 1 + 2);
    CHECK_EQ(usplit_rsp_len(USPLIT_OP_PAGE), 4 + 3 + 2);
    CHECK_EQ(usplit_rsp_len(USPLIT_OP_STATUS), 4 + 11 + 2);
    CHECK_EQ(usplit_rsp_len(USPLIT_OP_END), 6);
    CHECK_EQ(usplit_rsp_len(USPLIT_OP_COMMIT), 6);
    CHECK_EQ(usplit_rsp_len(USPLIT_OP_ABORT), 6);
    CHECK_EQ(usplit_rsp_len(USPLIT_OP_COUNT), 0);
    CHECK_EQ(usplit_rsp_len(0xFF), 0);
    for (uint8_t op = 0; op < USPLIT_OP_COUNT; op++) CHECK(usplit_rsp_len(op) <= USPLIT_MSG_MAX);
    CHECK_EQ(USPLIT_PAGE_DATA_MAX, 24);
    CHECK_EQ(USPLIT_BEGIN_DATA_MAX, 26);

    // Payload-less ops.
    const uint8_t bare[] = {USPLIT_OP_INFO, USPLIT_OP_STATUS, USPLIT_OP_END, USPLIT_OP_ABORT};
    for (size_t i = 0; i < sizeof(bare); i++) {
        CHECK_EQ(usplit_req_build(buf, bare[i], 9, NULL, 0), 4);
        CHECK_EQ(usplit_req_parse(buf, 4, &r), UPDATE_OK);
        CHECK(r.op == bare[i] && r.seq == 9 && r.payload_len == 0);
        CHECK_EQ(usplit_req_build(buf, bare[i], 9, data, 1), 0); // no payload allowed
    }
    CHECK_EQ(usplit_req_build(buf, USPLIT_OP_COUNT, 0, NULL, 0), 0); // unknown op

    // BEGIN: the signed manifest in fragments of at most 26 bytes.
    CHECK_EQ(usplit_req_begin(buf, 1, 0, data, 26), 32);
    CHECK_EQ(usplit_req_parse(buf, 32, &r), UPDATE_OK);
    CHECK(r.op == USPLIT_OP_BEGIN && r.off == 0 && r.n == 26 && memcmp(r.data, data, 26) == 0);
    CHECK_EQ(usplit_req_begin(buf, 1, 0, data, 27), 0);
    CHECK_EQ(usplit_req_begin(buf, 1, 0, data, 0), 0);
    CHECK_EQ(usplit_req_begin(buf, 1, 160, data, 12), 18); // ends at 172
    CHECK_EQ(usplit_req_begin(buf, 1, 160, data, 13), 0);  // past the 172 bytes

    // PAGE: at most 24 bytes, never across a 256-byte page.
    CHECK_EQ(usplit_req_page(buf, 2, 0x012340, data, 24), 32);
    CHECK_EQ(usplit_req_parse(buf, 32, &r), UPDATE_OK);
    CHECK(r.op == USPLIT_OP_PAGE && r.seq == 2 && r.off == 0x012340 && r.n == 24 && memcmp(r.data, data, 24) == 0);
    CHECK_EQ(usplit_req_page(buf, 2, 0, data, 25), 0);
    CHECK_EQ(usplit_req_page(buf, 2, 0, data, 0), 0);
    CHECK_EQ(usplit_req_page(buf, 2, 240, data, 16), 24); // ends on the page boundary
    CHECK_EQ(usplit_req_page(buf, 2, 240, data, 17), 0);  // crosses it
    CHECK_EQ(usplit_req_page(buf, 2, 0x1000000, data, 1), 0);

    // COMMIT carries the CRC's low 16 bits.
    CHECK_EQ(usplit_req_commit(buf, 3, 0xBEEF), 6);
    CHECK_EQ(usplit_req_parse(buf, 6, &r), UPDATE_OK);
    CHECK_EQ(r.crc_lo16, 0xBEEF);

    // Every single-bit error in a frame is caught; so are truncation and extra bytes.
    uint8_t len = usplit_req_page(buf, 7, 0x100, data, 24);
    for (int i = 0; i < len * 8; i++) {
        uint8_t f[USPLIT_MSG_MAX];
        memcpy(f, buf, len);
        f[i / 8] ^= (uint8_t)(1u << (i % 8));
        CHECK_EQ(usplit_req_parse(f, len, &r), UPDATE_INVALID);
    }
    for (uint8_t l = 0; l < len; l++) CHECK_EQ(usplit_req_parse(buf, l, &r), UPDATE_INVALID);
    uint8_t big[40] = {0};
    memcpy(big, buf, len);
    CHECK_EQ(usplit_req_parse(big, 33, &r), UPDATE_INVALID);
    // A good CRC over a frame that breaks the op's rules: still INVALID.
    uint8_t frame[USPLIT_MSG_MAX] = {USPLIT_OP_PAGE, 0, 0xF0, 0, 0, 17}; // 0xF0 + 17 crosses a page
    uint8_t blen                  = 6 + 17;
    uint16_t crc                  = usplit_crc16(frame, blen);
    frame[blen]                   = crc & 0xFF;
    frame[blen + 1]               = crc >> 8;
    CHECK_EQ(usplit_req_parse(frame, blen + 2, &r), UPDATE_INVALID);

    // Responses: built and checked against the request's op and seq.
    usplit_rsp_t rs;
    uint8_t      pl[15];
    for (int i = 0; i < 15; i++) pl[i] = (uint8_t)i;
    CHECK_EQ(usplit_rsp_build(buf, sizeof(buf), UPDATE_OK, USPLIT_OP_INFO, 5, UPDATE_STATE_IDLE, pl), 21);
    CHECK_EQ(usplit_rsp_parse(buf, 21, USPLIT_OP_INFO, 5, &rs), UPDATE_OK);
    CHECK(rs.status == UPDATE_OK && rs.state == UPDATE_STATE_IDLE && rs.payload_len == 15 && memcmp(rs.payload, pl, 15) == 0);
    CHECK_EQ(usplit_rsp_parse(buf, 21, USPLIT_OP_INFO, 6, &rs), UPDATE_INVALID);   // stale: another seq
    CHECK_EQ(usplit_rsp_parse(buf, 21, USPLIT_OP_STATUS, 5, &rs), UPDATE_INVALID); // another op
    CHECK_EQ(usplit_rsp_parse(buf, 20, USPLIT_OP_INFO, 5, &rs), UPDATE_INVALID);   // length
    buf[7] ^= 0x40;
    CHECK_EQ(usplit_rsp_parse(buf, 21, USPLIT_OP_INFO, 5, &rs), UPDATE_INVALID); // CRC
    CHECK_EQ(usplit_rsp_build(buf, 20, UPDATE_OK, USPLIT_OP_INFO, 5, 0, pl), 0); // no room
    CHECK_EQ(usplit_rsp_build(buf, sizeof(buf), UPDATE_OK, 0x7F, 5, 0, pl), 0);  // unknown op
    uint8_t zeros[32] = {0};
    CHECK_EQ(usplit_rsp_parse(zeros, 6, USPLIT_OP_END, 0, &rs), UPDATE_INVALID); // all zeros fail the CRC
}

// ---- PAGE fragments into pages --------------------------------------------------------

static void test_pages(void) {
    static usplit_pages_t a;
    uint8_t               img[512 + 256];
    for (size_t i = 0; i < sizeof(img); i++) img[i] = (uint8_t)(i * 7 + 3);
    usplit_pages_init(&a, 512);
    uint32_t done, off = 0;
    int      pages = 0;
    // Fragments of 24 bytes, shortened at each page end.
    while (off < 512) {
        uint8_t n = 24;
        if ((off % 256) + n > 256) n = (uint8_t)(256 - off % 256);
        CHECK_EQ(usplit_pages_accept(&a, off, img + off, n, &done), UPDATE_OK);
        off += n;
        if (off % 256 == 0) {
            CHECK_EQ(done, off - 256);
            CHECK(memcmp(a.page, img + off - 256, 256) == 0);
            pages++;
        } else {
            CHECK_EQ(done, UINT32_MAX);
        }
    }
    CHECK_EQ(pages, 2);
    CHECK_EQ(a.next, 512);
    // A repeat is acknowledged and never written again (idempotent by offset).
    uint8_t junk[24];
    memset(junk, 0x5A, sizeof(junk));
    uint8_t before[256];
    memcpy(before, a.page, 256);
    CHECK_EQ(usplit_pages_accept(&a, 256 + 24, junk, 24, &done), UPDATE_OK);
    CHECK_EQ(done, UINT32_MAX);
    CHECK(memcmp(a.page, before, 256) == 0);
    CHECK_EQ(a.next, 512);
    // Past the image: OVERRUN.
    CHECK_EQ(usplit_pages_accept(&a, 512, img, 24, &done), UPDATE_OVERRUN);
    // Mid-page repeats, gaps and partial overlaps.
    usplit_pages_init(&a, 256);
    CHECK_EQ(usplit_pages_accept(&a, 0, img, 24, &done), UPDATE_OK);
    CHECK_EQ(usplit_pages_accept(&a, 0, junk, 24, &done), UPDATE_OK); // repeat
    CHECK(memcmp(a.page, img, 24) == 0);
    CHECK_EQ(usplit_pages_accept(&a, 48, img + 48, 24, &done), UPDATE_OUT_OF_ORDER); // gap
    CHECK_EQ(usplit_pages_accept(&a, 12, img + 12, 24, &done), UPDATE_OUT_OF_ORDER); // overlaps the end
    CHECK_EQ(usplit_pages_accept(&a, 24, img + 24, 0, &done), UPDATE_INVALID);
    CHECK_EQ(usplit_pages_accept(&a, 24, img + 24, 25, &done), UPDATE_INVALID);
    CHECK_EQ(usplit_pages_accept(&a, 248, img, 16, &done), UPDATE_INVALID); // crosses a page
    CHECK_EQ(a.next, 24);
    // An image shorter than the fragment that would end it.
    usplit_pages_init(&a, 0);
    CHECK_EQ(usplit_pages_accept(&a, 0, img, 24, &done), UPDATE_OVERRUN);
}

// ---- the slave's handler ----------------------------------------------------------------

static uint8_t seq;

// One RPC: request frame in, response checked against (op, seq).
static update_status_t rpc(uint8_t *req, uint8_t req_len, usplit_rsp_t *rs, uint8_t *out) {
    uint8_t op = req[0], s = req[1];
    memset(out, 0xEE, 40);
    update_split_slave_rpc(req, req_len, out, USPLIT_MSG_MAX);
    for (int i = USPLIT_MSG_MAX; i < 40; i++) CHECK_EQ(out[i], 0xEE); // never past out_len
    return usplit_rsp_parse(out, usplit_rsp_len(op), op, s, rs);
}

static void test_slave(void) {
    update_split_host_reset();
    uint8_t      req[USPLIT_MSG_MAX], out[40], data[26] = {0};
    usplit_rsp_t rs;
    uint8_t      len;

    // INFO: this half's build, the INFO flags.
    len = usplit_req_build(req, USPLIT_OP_INFO, ++seq, NULL, 0);
    port_info_calls = 0;
    CHECK_EQ(rpc(req, len, &rs, out), UPDATE_OK);
    CHECK_EQ(rs.status, UPDATE_OK);
    CHECK_EQ(rs.state, UPDATE_STATE_IDLE);
    CHECK_EQ(rs.payload[0], UPDATE_PROTOCOL_VERSION);
    CHECK_EQ(rs.payload[1], UPDATE_HAND_RIGHT);
    CHECK_EQ(rs.payload[2], UPDATE_POINTING_PMW3389);
    CHECK_EQ(rs.payload[3] | rs.payload[4] << 8 | rs.payload[5] << 16 | (uint32_t)rs.payload[6] << 24, 2001);
    CHECK_EQ(rs.payload[7] | rs.payload[8] << 8 | rs.payload[9] << 16 | (uint32_t)rs.payload[10] << 24, 0x3b2ca7e1);
    CHECK_EQ(rs.payload[11], SVAL_UPDATE_STORAGE_FORMAT);
    CHECK_EQ(rs.payload[12] | rs.payload[13] << 8, SVAL_UPDATE_SECURITY_EPOCH);
    CHECK_EQ(rs.payload[14], 0x11);
    CHECK_EQ(port_info_calls, 1);
    // The same frame again (its answer was lost): the cached answer, not run again.
    uint8_t first[USPLIT_MSG_MAX];
    memcpy(first, out, USPLIT_MSG_MAX);
    CHECK_EQ(rpc(req, len, &rs, out), UPDATE_OK);
    CHECK(memcmp(first, out, USPLIT_MSG_MAX) == 0);
    CHECK_EQ(port_info_calls, 1);
    // A new seq runs again.
    len = usplit_req_build(req, USPLIT_OP_INFO, ++seq, NULL, 0);
    CHECK_EQ(rpc(req, len, &rs, out), UPDATE_OK);
    CHECK_EQ(port_info_calls, 2);

    // STATUS and ABORT.
    len = usplit_req_build(req, USPLIT_OP_STATUS, ++seq, NULL, 0);
    CHECK_EQ(rpc(req, len, &rs, out), UPDATE_OK);
    CHECK(rs.status == UPDATE_OK && rs.payload_len == USPLIT_STATUS_PAYLOAD && rs.payload[0] == UPDATE_OK);
    len = usplit_req_build(req, USPLIT_OP_ABORT, ++seq, NULL, 0);
    CHECK_EQ(rpc(req, len, &rs, out), UPDATE_OK);
    CHECK_EQ(rs.status, UPDATE_OK);

    // Stage 1: BEGIN, PAGE, END and COMMIT are checked, then refused; nothing changes.
    len = usplit_req_begin(req, ++seq, 0, data, 26);
    CHECK_EQ(rpc(req, len, &rs, out), UPDATE_OK);
    CHECK_EQ(rs.status, UPDATE_UNSUPPORTED);
    len = usplit_req_page(req, ++seq, 0, data, 24);
    CHECK_EQ(rpc(req, len, &rs, out), UPDATE_OK);
    CHECK_EQ(rs.status, UPDATE_UNSUPPORTED);
    len = usplit_req_build(req, USPLIT_OP_END, ++seq, NULL, 0);
    CHECK_EQ(rpc(req, len, &rs, out), UPDATE_OK);
    CHECK_EQ(rs.status, UPDATE_UNSUPPORTED);
    len = usplit_req_commit(req, ++seq, 0x1234);
    CHECK_EQ(rpc(req, len, &rs, out), UPDATE_OK);
    CHECK_EQ(rs.status, UPDATE_UNSUPPORTED);
    CHECK_EQ(update_split_slave_state(), UPDATE_STATE_IDLE);

    // Same seq, different bytes (a new request that reused the seq): runs.
    len = usplit_req_build(req, USPLIT_OP_INFO, seq, NULL, 0);
    port_info_calls = 0;
    CHECK_EQ(rpc(req, len, &rs, out), UPDATE_OK);
    CHECK_EQ(port_info_calls, 1);

    // A corrupted frame: answered INVALID (op and seq echoed, so the master
    // can tell), never cached, and the good retry with the same seq then runs.
    len = usplit_req_build(req, USPLIT_OP_INFO, ++seq, NULL, 0);
    req[len - 1] ^= 1;
    port_info_calls = 0;
    CHECK_EQ(rpc(req, len, &rs, out), UPDATE_OK);
    CHECK_EQ(rs.status, UPDATE_INVALID);
    CHECK_EQ(port_info_calls, 0);
    req[len - 1] ^= 1;
    CHECK_EQ(rpc(req, len, &rs, out), UPDATE_OK);
    CHECK_EQ(rs.status, UPDATE_OK);
    CHECK_EQ(port_info_calls, 1);
    // A PAGE that breaks the rules (crosses a page) with a good CRC: INVALID.
    uint8_t pl[4 + 17] = {0xF0, 0, 0, 17};
    uint8_t f[USPLIT_MSG_MAX] = {USPLIT_OP_PAGE, ++seq};
    memcpy(f + 2, pl, sizeof(pl));
    uint16_t crc = usplit_crc16(f, 2 + sizeof(pl));
    f[2 + sizeof(pl)]     = crc & 0xFF;
    f[2 + sizeof(pl) + 1] = crc >> 8;
    CHECK_EQ(rpc(f, (uint8_t)(4 + sizeof(pl)), &rs, out), UPDATE_OK);
    CHECK_EQ(rs.status, UPDATE_INVALID);

    // An unknown op, or a frame too short to name one: nothing but zeros back.
    uint8_t u[4] = {0x7F, 1, 0, 0};
    crc  = usplit_crc16(u, 2);
    u[2] = crc & 0xFF;
    u[3] = crc >> 8;
    memset(out, 0xEE, sizeof(out));
    update_split_slave_rpc(u, 4, out, USPLIT_MSG_MAX);
    for (int i = 0; i < USPLIT_MSG_MAX; i++) CHECK_EQ(out[i], 0);
    update_split_slave_rpc(u, 0, out, USPLIT_MSG_MAX);
    for (int i = 0; i < USPLIT_MSG_MAX; i++) CHECK_EQ(out[i], 0);
    // An out buffer shorter than the response: zeros, never past it.
    len = usplit_req_build(req, USPLIT_OP_INFO, ++seq, NULL, 0);
    memset(out, 0xEE, sizeof(out));
    update_split_slave_rpc(req, len, out, 8);
    for (int i = 0; i < 8; i++) CHECK_EQ(out[i], 0);
    for (int i = 8; i < 40; i++) CHECK_EQ(out[i], 0xEE);

    // Random frames of every length: no crash, no write past out_len (ASan),
    // and every answer is either zeros or a well-formed response.
    uint32_t rng = 1;
    for (int t = 0; t < 20000; t++) {
        uint8_t in[USPLIT_MSG_MAX];
        uint8_t l = (uint8_t)(t % (USPLIT_MSG_MAX + 1));
        for (int i = 0; i < l; i++) {
            rng   = rng * 1103515245u + 12345u;
            in[i] = (uint8_t)(rng >> 16);
        }
        if (l >= 1 && t % 3 == 0) in[0] %= USPLIT_OP_COUNT; // mostly known ops
        if (l >= 4 && t % 2 == 0) {                         // half with a good CRC
            uint16_t c  = usplit_crc16(in, l - 2);
            in[l - 2]   = c & 0xFF;
            in[l - 1]   = c >> 8;
        }
        uint8_t o[USPLIT_MSG_MAX];
        update_split_slave_rpc(in, l, o, USPLIT_MSG_MAX);
        bool zero = true;
        for (int i = 0; i < USPLIT_MSG_MAX; i++) zero &= o[i] == 0;
        if (!zero) {
            CHECK(l >= 2 && o[1] < USPLIT_OP_COUNT);
            if (l >= 2 && o[1] < USPLIT_OP_COUNT) CHECK_EQ(usplit_rsp_parse(o, usplit_rsp_len(o[1]), o[1], o[2], &rs), UPDATE_OK);
        }
    }
    CHECK_EQ(update_split_slave_state(), UPDATE_STATE_IDLE);
}

int main(void) {
    test_pause();
    test_checks();
    test_presence();
    test_wire();
    test_pages();
    test_slave();
    printf("split tests: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
