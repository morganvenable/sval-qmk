// Host test: the native touchpad's contact path and split link
// (keyboards/svalboard/azoteq/multitouch.c) together with ptp.c.
//
//  - the half without USB: its news counter moves only on news, and stays
//    off its pointing report until a USB half asks for it ("hints ok");
//  - the snapshot reply fits the 32-byte RPC and decodes to the same frame;
//  - the USB half fetches the other pad at once on news, every BACKUP ms
//    while it has contacts or is not armed, every IDLE ms while it is quiet,
//    and only the heartbeat for a half with no snapshot or older firmware;
//  - nothing is fetched for a hand without a pad, or while an update runs
//    (and the other pad's contacts lift then);
//  - touchpad use counts as input activity; a chip reset is set up again,
//    never in a loop; the pad mouse is suppressed per hand;
//  - the dev command (owner) refuses everything but mouse and native.
#define SVAL_MT_HOST_TEST
#define SVAL_PTP_HOST_TEST
#define SVAL_TOUCH_DEV
#define SVAL_TOUCH_OWNER_DEFAULT 3
#include "../../keyboards/svalboard/azoteq/multitouch.c" // brings mt_test_platform.h
#include "../../keyboards/svalboard/azoteq/ptp.c"

// ------------------------------------------------------------- helpers

static void raw_sample(uint8_t *raw, bool finger, uint16_t x, uint16_t y) {
    memset(raw, 0, 45);
    raw[0] = 8;
    if (finger) {
        raw[5]  = 1;      // one finger
        raw[10] = x >> 8; // slot 0: absolute X (big endian), Y, strength, area
        raw[11] = x & 0xFF;
        raw[12] = y >> 8;
        raw[13] = y & 0xFF;
        raw[15] = 40;
        raw[16] = 20;
    }
}

// The other half as the test plays it: the reply its firmware packs, built
// here independently (checked against pack_snapshot below).
static uint8_t  peer_seq, peer_epoch    = 1, peer_age;
static bool     peer_finger, peer_valid = true;
static uint16_t peer_x, peer_y;
static uint8_t  peer_last_req[3];

static void encode_peer(uint8_t *out) {
    memset(out, 0, 32);
    out[0] = 0xA7;
    out[1] = (peer_valid ? 1 : 0) | 2;
    out[2] = peer_age;
    out[4] = peer_finger ? 1 : 0;
    out[5] = peer_seq;
    out[6] = peer_epoch;
    if (peer_finger) {
        out[7]     = 1;
        uint32_t w = peer_x | (uint32_t)peer_y << 11 | 20u << 22;
        put32(out + 8, w);
    }
}
static void peer(uint8_t in_len, const uint8_t *in, uint8_t out_len, uint8_t *out) {
    assert(out_len == 32);
    memcpy(peer_last_req, in, in_len < 3 ? in_len : 3);
    if (in[0] != 0xA7) {
        memset(out, 0, 32);
        return;
    }
    encode_peer(out);
}
// Firmware without the op: Scan Lab answers an unknown op with zeros.
static void old_peer(uint8_t in_len, const uint8_t *in, uint8_t out_len, uint8_t *out) {
    (void)in_len;
    (void)in;
    memset(out, 0, out_len);
}

static uint8_t hint; // what the other half's pointing report carries
static bool    hint_on = true;

static void news(void) {
    hint = 0x80 | ((hint + 1) & 0x7F);
    peer_seq++;
}
static void set_peer(bool finger, uint16_t x, uint16_t y) {
    peer_finger = finger;
    peer_x      = x;
    peer_y      = y;
    news();
}

// One millisecond of the USB half's main loop.
static void tick(void) {
    clock_ms += 1;
    mt_test_us += 1000;
    ptp_test_us += 1000;
    sval_mt_remote_hint(hint_on ? hint : 0);
    sval_mt_task();
}
static void run(uint32_t ms) {
    while (ms--)
        tick();
}

static const uint8_t *latest(void) {
    return ptp_sent_count ? ptp_sent[ptp_sent_count - 1] : NULL;
}
// The USB half is the right one here, so the other pad is the left (pad 0).
static bool left_down(void) {
    const uint8_t *r = latest();
    if (!r) return false;
    for (int i = 0; i < r[33]; i++)
        if ((r[1 + 6 * i] & 2) && ptp_get16(r + 3 + 6 * i) < SVAL_PTP_PAD_STRIDE) return true;
    return false;
}

static bool ptp_paused(void) {
    uint8_t st[SVAL_PTP_STATUS_SIZE];
    sval_ptp_status(st);
    return st[4] & 128;
}

// ------------------------------------------------------------ the tests

static void test_other_half(void) {
    uint8_t raw[45], req[2] = {0xA7, 0}, reply[32];
    master    = false;
    left_half = true;
    clock_ms  = 10;
    // No USB half has asked: the news counter stays off the pointing report.
    raw_sample(raw, true, 300, 400);
    sval_mt_sample(raw, true);
    assert(sval_mt_split_hint() == 0);
    assert(sval_mt_rpc(2, req, 32, reply) && reply[0] == 0xA7 && (reply[1] & 1));
    assert(sval_mt_split_hint() == 0); // asked without "hints ok"
    req[1] = 1;
    assert(sval_mt_rpc(2, req, 32, reply));
    uint8_t h0 = sval_mt_split_hint();
    assert(h0 & 0x80);
    // The snapshot decodes to the frame the pad produced, and the test's own
    // encoder agrees with the firmware's.
    uint8_t frame[56];
    unpack_snapshot(reply, 0, frame);
    assert(frame[0] == 1 && frame[1] == 0 && frame[3] == 1);
    assert(ptp_get16(frame + 17) == 300 && ptp_get16(frame + 19) == 400 && frame[23] == 20 && frame[16] == snapshot[16]);
    peer_finger = true, peer_x = 300, peer_y = 400, peer_seq = reply[5], peer_epoch = reply[6], peer_age = reply[2];
    uint8_t mine[32];
    encode_peer(mine);
    mine[7] = reply[7]; // the lifetime ID is the firmware's
    assert(!memcmp(mine, reply, 32));
    // The largest coordinates survive the 11-bit packing.
    raw_sample(raw, true, 2047, 1791);
    sval_mt_sample(raw, true);
    assert(sval_mt_rpc(2, req, 32, reply));
    unpack_snapshot(reply, 0, frame);
    assert(ptp_get16(frame + 17) == 2047 && ptp_get16(frame + 19) == 1791);
    // The counter moves only on news: contacts, lifts and flags.
    uint8_t h = sval_mt_split_hint();
    sval_mt_sample(raw, true); // a stationary finger is still news
    assert(sval_mt_split_hint() != h);
    raw_sample(raw, false, 0, 0);
    sval_mt_sample(raw, true); // the lift is news
    h = sval_mt_split_hint();
    sval_mt_sample(raw, true);
    sval_mt_sample(raw, true);
    assert(sval_mt_split_hint() == h); // then quiet
    sval_mt_sample(raw, false);        // a failed read is news
    assert(sval_mt_split_hint() != h);
    for (int i = 0; i < 300; i++) {
        raw_sample(raw, true, 10, 10);
        sval_mt_sample(raw, true);
        assert(sval_mt_split_hint() & 0x80); // wraps, bit 7 stays
    }
    // A USB half that stops asking: the counter leaves the report after a second.
    clock_ms += 1001;
    assert(sval_mt_split_hint() == 0);
    // Other ops are not this file's.
    uint8_t other[3] = {0x10, 0, 0};
    assert(!sval_mt_rpc(3, other, 32, reply));
    puts("  other half: counter only after hints ok, only on news; 32-byte snapshot round-trips");
}

static void usb_half(void) {
    master    = true;
    left_half = false;
    connected = true;
    busy      = false;
    clock_ms += 1000;
    hands_with_pad = 3;
    other_half     = peer;
    peer_valid     = true;
    peer_age       = 2;
    sval_ptp_test_reset();
    sval_mt_init();
    uint8_t mode3[2] = {5, 3};
    assert(sval_ptp_set_report(3, 5, mode3, 2));
    pads_checked = 0;
    pad_mask = pad_hands = 0;
    remote_seen = remote_lost = remote_hints = remote_empty = last_ok = false;
    uint8_t raw[45];
    raw_sample(raw, false, 0, 0);
    run(1);
    sval_mt_sample(raw, true); // the local pad shows all-up
    set_peer(false, 0, 0);
    run(SVAL_PTP_REMOTE_BACKUP_MS * 3);
    sval_mt_sample(raw, true);
    run(SVAL_PTP_REMOTE_BACKUP_MS * 2);
    assert(pads[0].armed && pads[1].armed && sval_ptp_pad_quiet(0) && remote_hints);
}

static void test_fetch_gating(void) {
    usb_half();
    assert(peer_last_req[0] == 0xA7 && peer_last_req[1] == 1); // asks for hints

    // Quiet: only the heartbeat.
    unsigned before = rpc_calls;
    run(1000);
    unsigned idle = rpc_calls - before;
    assert(idle >= 3 && idle <= 5);

    // Touch-down on the other pad: fetched on the next pass.
    set_peer(true, 50, 60);
    before = rpc_calls;
    tick();
    assert(rpc_calls == before + 1);
    run(9);
    assert(left_down());
    // Its contact is stamped with the snapshot's age: read 2 ms before the fetch.
    assert(pads[0].valid_us == ptp_test_us - 9000 - 2000);

    // A stationary finger: each new sample is fetched once.
    before = rpc_calls;
    for (int i = 0; i < 10; i++) {
        set_peer(true, 50, 60);
        run(8);
    }
    assert(rpc_calls - before == 10);

    // A lost counter change while a finger is down: the backup fetch runs.
    before = rpc_calls;
    run(40);
    assert(rpc_calls - before >= 40 / SVAL_PTP_REMOTE_BACKUP_MS - 1);

    // The lift is news and is delivered at once.
    set_peer(false, 0, 0);
    run(5);
    assert(!left_down());
    run(20);
    assert(sval_ptp_pad_quiet(0));

    // Finger down, then the link dies: lifted within the stale time.
    set_peer(true, 70, 80);
    run(9);
    assert(left_down());
    connected     = false;
    uint32_t died = clock_ms;
    while (left_down()) {
        tick();
        assert(clock_ms - died <= SVAL_PTP_STALE_MS + SVAL_PTP_REMOTE_BACKUP_MS + 10);
    }
    connected = true;
    set_peer(false, 0, 0);
    run(SVAL_PTP_REMOTE_BACKUP_MS * 3);
    assert(pads[0].armed && sval_ptp_pad_quiet(0));
    puts("  fetch gating: news at once, heartbeat when quiet, backup while down, lifts, dead link lifts");
}

static void test_no_snapshot(void) {
    // The other half answers but has no sample (its pad not read yet): treat
    // it as no pad, heartbeat only.
    usb_half();
    peer_valid = false;
    news();
    run(20);
    unsigned before = rpc_calls;
    run(1000);
    assert(rpc_calls - before <= 5 && remote_empty);
    peer_valid = true;
    news();
    run(SVAL_PTP_REMOTE_IDLE_MS + 5);
    assert(!remote_empty);

    // Firmware without the op (Scan Lab's zeros): heartbeat only, nothing fed.
    usb_half();
    assert(sval_mt_suppress_hand(0)); // the other pad answered: the touchpad has it
    other_half = old_peer;
    run(SVAL_PTP_REMOTE_IDLE_MS + 5);
    before          = rpc_calls;
    unsigned frames = ptp_sent_count;
    run(1000);
    assert(rpc_calls - before <= 5 && remote_empty && !remote_hints && ptp_sent_count == frames);
    // Its pad is never handed to the touchpad: it stays a mouse.
    assert(!sval_mt_suppress_hand(0) && sval_mt_suppress_hand(1) && !sval_mt_os_touchpad(0));
    other_half = peer;
    puts("  no snapshot: a half with nothing to send, or older firmware, gets only the heartbeat");
}

static void test_no_pad_there(void) {
    // The other hand has a trackball: never fetched, never suppressed.
    usb_half();
    hands_with_pad = 2; // right only (the USB half)
    run(MT_PADS_CHECK_MS + 1);
    unsigned before = rpc_calls;
    run(2000);
    assert(rpc_calls == before);
    assert(!sval_mt_suppress_hand(0) && sval_mt_suppress_hand(1));
    // No pad anywhere: the touchpad sends nothing and suppresses nothing.
    hands_with_pad = 0;
    run(MT_PADS_CHECK_MS + 1);
    assert(!sval_mt_suppress_hand(0) && !sval_mt_suppress_hand(1) && !sval_ptp_wants_frames());
    hands_with_pad = 1; // the left pad only (the other half's)
    run(MT_PADS_CHECK_MS + 1);
    assert(sval_mt_suppress_hand(0) && !sval_mt_suppress_hand(1));
    hands_with_pad = 3;
    run(MT_PADS_CHECK_MS + 1);
    puts("  pads per hand: a hand without a pad is never fetched or suppressed");
}

static void test_update(void) {
    usb_half();
    set_peer(true, 90, 95);
    run(9);
    assert(left_down());
    busy            = true; // an update starts
    unsigned before = rpc_calls;
    run(50);
    assert(rpc_calls == before && !left_down() && ptp_paused());
    busy = false;
    set_peer(false, 0, 0);
    run(SVAL_PTP_REMOTE_IDLE_MS * 2);
    puts("  update: no split fetches, the other pad's contacts lift, reports pause");
}

static void test_activity(void) {
    usb_half();
    unsigned before = activity_calls;
    run(100);
    assert(activity_calls == before); // nothing down
    set_peer(true, 10, 20);
    run(9);
    before = activity_calls;
    run(20);
    assert(activity_calls - before >= 19);
    set_peer(false, 0, 0);
    run(20);
    puts("  activity: touchpad use is input activity");
}

static void test_reinit(void) {
    usb_half();
    uint8_t raw[45];
    raw_sample(raw, false, 0, 0);
    raw[3] = 0x80; // SHOW_RESET: the chip reset since init
    sval_mt_sample(raw, true);
    run(1);
    assert(reinit_calls == 1);
    sval_mt_sample(raw, true);
    run(100);
    assert(reinit_calls == 1); // not again within MT_REINIT_MIN_MS
    run(MT_REINIT_MIN_MS);
    assert(reinit_calls == 2);
    puts("  chip reset: set up again, at most every 5 s");
}

static void test_owner(void) {
    usb_half();
    uint8_t d[64] = {0};
    d[6]          = 0x42;
    d[7]          = 1;
    d[8]          = SVAL_TOUCH_OWNER_HOST; // retired
    sval_mt_command(d, 64);
    assert(d[6] == 0xC2 && d[7] == 2 && d[8] == SVAL_TOUCH_OWNER_NATIVE);
    memset(d, 0, 64);
    d[6] = 0x42;
    d[7] = 1;
    d[8] = SVAL_TOUCH_OWNER_MOUSE;
    sval_mt_command(d, 64);
    assert(d[7] == 0 && d[8] == SVAL_TOUCH_OWNER_MOUSE && (d[9] & 3) == 3);
    assert(d[10] == 0); // the native owner is off
    run(5);
    assert(!sval_ptp_suppress_mouse());
    memset(d, 0, 64);
    d[6] = 0x42;
    d[7] = 1;
    d[8] = SVAL_TOUCH_OWNER_NATIVE;
    sval_mt_command(d, 64);
    assert(d[7] == 0 && d[10] == 1 && d[10 + 16] == 3); // pads: left and right
    puts("  owner command: mouse and native only; status v2 reports both pads");
}

int main(void) {
    puts("native PTP split and contact path tests:");
    test_other_half();
    test_fetch_gating();
    test_no_snapshot();
    test_no_pad_there();
    test_update();
    test_activity();
    test_reinit();
    test_owner();
    puts("native PTP split and contact path: all passed");
    return 0;
}
