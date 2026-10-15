// Host test: the native touchpad fetches the other half's pad only when it
// can hold something new. The half without USB moves a news counter only on
// contacts, lifts and flags; the USB half fetches at once on news, every
// BACKUP ms while that pad has contacts or is not armed, every IDLE ms while
// it is quiet, and every POLL ms when the other half sends no counter (older
// firmware). Contacts never stick: lifts are news, and a dead link still
// lifts within the stale timeout.
#define SVAL_MT_HOST_TEST
#define SVAL_MT_SPLIT
#define SVAL_NATIVE_PTP
#define SVAL_TOUCH_OWNER_DEFAULT 3
#define SVAL_MT_DEFAULT_PERIOD_MS 8
#define SVAL_PTP_HOST_TEST
#include "../../keyboards/svalboard/azoteq/multitouch.c" // brings mt_test_platform.h
#include "../../keyboards/svalboard/azoteq/ptp.c"
uint32_t sval_mt_rp_abort_source;

static uint8_t hint;          // what the other half's pointing report carries
static bool    hint_on = true; // false: an older other half (boot_x stays 0)

// One millisecond of the USB half's main loop: the split pointing sync hands
// over the counter, then housekeeping runs.
static void tick(void) {
    clock_ms += 1;
    ptp_test_us += 1000;
    sval_mt_remote_hint(hint_on ? hint : 0);
    sval_mt_task();
}
static void run(uint32_t ms) {
    while (ms--) tick();
}

static uint16_t remote_seq;
// The other half's snapshot (pad 0, the left), as its RPC returns it.
static void snapshot(bool finger, uint16_t x, uint16_t y) {
    memset(rpc_data, 0, sizeof(rpc_data));
    rpc_data[0] = 0xA7;
    rpc_data[1] = 1;
    uint8_t *f  = rpc_data + 4;
    f[0]        = 1;
    f[1]        = 0;
    put16(f + 4, ++remote_seq);
    put16(f + 6, 56);
    put32(f + 12, 1);
    if (finger) {
        f[3]  = 1;
        f[16] = 1;
        put16(f + 17, x);
        put16(f + 19, y);
        put16(f + 21, 40);
        f[23] = 20;
    }
}
static void news(void) {
    hint = 0x80 | ((hint + 1) & 0x7F);
}

static const uint8_t *latest_sent(void) {
    return ptp_sent[ptp_sent_count - 1];
}
static bool left_down(void) {
    const uint8_t *r = latest_sent();
    for (int i = 0; i < r[33]; i++)
        if ((r[1 + 6 * i] & 2) && ptp_get16(r + 3 + 6 * i) < SVAL_PTP_PAD_STRIDE) return true;
    return false;
}

static void raw_sample(uint8_t *raw, bool finger) {
    memset(raw, 0, 45);
    raw[0] = 8;
    if (finger) {
        raw[5]  = 1;
        raw[10] = 1;
        raw[11] = 2;
        raw[15] = 40;
    }
}

int main(void) {
    // --- The half without USB: the counter moves only on news. ---
    uint8_t raw[45];
    master   = false;
    clock_ms = 10;
    uint8_t h0 = sval_mt_split_hint();
    assert(h0 & 0x80);
    for (int i = 0; i < 20; i++) {
        raw_sample(raw, false);
        sval_mt_sample(raw, true);
    }
    assert(sval_mt_split_hint() == h0); // idle: no news, no fetches
    raw_sample(raw, true);
    sval_mt_sample(raw, true);
    uint8_t h1 = sval_mt_split_hint();
    assert(h1 != h0 && (h1 & 0x80));
    sval_mt_sample(raw, true); // a stationary finger is still news
    uint8_t h2 = sval_mt_split_hint();
    assert(h2 != h1);
    raw_sample(raw, false);
    sval_mt_sample(raw, true); // the lift is news
    uint8_t h3 = sval_mt_split_hint();
    assert(h3 != h2);
    sval_mt_sample(raw, true);
    sval_mt_sample(raw, true);
    assert(sval_mt_split_hint() == h3); // then quiet again
    sval_mt_sample(raw, false);          // a failed read is news
    assert(sval_mt_split_hint() != h3);
    for (int i = 0; i < 300; i++) sval_mt_sample(raw, true); // the counter wraps, bit 7 stays
    raw_sample(raw, true);
    for (int i = 0; i < 300; i++) {
        sval_mt_sample(raw, true);
        assert(sval_mt_split_hint() & 0x80);
    }

    // --- The USB half. ---
    master    = true;
    connected = true;
    clock_ms  = 1000;
    sval_mt_init();
    uint8_t mode3[2] = {5, 3};
    assert(sval_ptp_set_report(3, 5, mode3, 2));
    raw_sample(raw, false);
    sval_mt_sample(raw, true); // the local pad shows all-up
    snapshot(false, 0, 0);
    hint = 0x80;

    // Not armed yet (reports just turned on): polled every BACKUP ms, without
    // news, until an all-up snapshot arms it. The other half's snapshot
    // sequence moves every sample even when nothing else does.
    run(1);
    unsigned before = rpc_calls;
    run(SVAL_PTP_REMOTE_BACKUP_MS * 3);
    assert(rpc_calls - before >= 2);
    snapshot(false, 0, 0);
    raw_sample(raw, false);
    sval_mt_sample(raw, true);
    run(SVAL_PTP_REMOTE_BACKUP_MS + 1);
    assert(pads[0].armed && pads[1].armed && sval_ptp_pad_quiet(0));

    // Quiet: only the heartbeat, one fetch a quarter second.
    before = rpc_calls;
    run(1000);
    unsigned idle = rpc_calls - before;
    assert(idle >= 3 && idle <= 5);
    printf("quiet other pad: %u fetches a second (was %d)\n", idle, 1000 / SVAL_PTP_REMOTE_POLL_MS);

    // Touch-down on the other pad: fetched on the very next pass.
    snapshot(true, 50, 60);
    news();
    before = rpc_calls;
    tick();
    assert(rpc_calls == before + 1);
    run(4);
    assert(left_down());

    // A stationary finger: each new sample is fetched once.
    before = rpc_calls;
    for (int i = 0; i < 10; i++) {
        snapshot(true, 50, 60);
        news();
        run(8);
    }
    assert(rpc_calls - before == 10);

    // A lost hint while a finger is down: BACKUP polling still runs.
    before = rpc_calls;
    run(100);
    assert(rpc_calls - before >= 100 / SVAL_PTP_REMOTE_BACKUP_MS - 1);

    // The lift is news and is delivered at once.
    snapshot(false, 0, 0);
    news();
    run(5);
    assert(!left_down());
    run(20);
    assert(sval_ptp_pad_quiet(0));

    // Finger down, then the link dies: lifted within the stale timeout.
    snapshot(true, 70, 80);
    news();
    run(5);
    assert(left_down());
    connected = false;
    uint32_t died = clock_ms;
    while (left_down()) {
        tick();
        assert(clock_ms - died <= SVAL_PTP_REMOTE_STALE_MS + SVAL_PTP_REMOTE_BACKUP_MS + 5);
    }
    printf("dead link lifted after %u ms\n", (unsigned)(clock_ms - died));
    connected = true;
    snapshot(false, 0, 0);
    news();
    run(SVAL_PTP_REMOTE_BACKUP_MS * 2);
    assert(pads[0].armed && sval_ptp_pad_quiet(0));

    // Quiet pad, link down past the stale timeout: nothing to lift, so the
    // pad stays armed, and a touch right after the link returns is reported.
    connected = false;
    run(SVAL_PTP_REMOTE_STALE_MS + 60);
    assert(pads[0].armed);
    connected = true;
    snapshot(true, 90, 95);
    news();
    run(5);
    assert(left_down());
    snapshot(false, 0, 0);
    news();
    run(20);
    assert(!left_down() && sval_ptp_pad_quiet(0));

    // An older other half sends no counter: every POLL ms, as before.
    hint_on = false;
    run(SVAL_PTP_REMOTE_HINT_LOST_MS);
    before = rpc_calls;
    run(100);
    unsigned old = rpc_calls - before;
    assert(old >= 100 / SVAL_PTP_REMOTE_POLL_MS - 1);
    snapshot(true, 40, 40); // and still delivers contacts
    run(SVAL_PTP_REMOTE_POLL_MS + 5);
    assert(left_down());
    snapshot(false, 0, 0);
    run(SVAL_PTP_REMOTE_POLL_MS + 5);
    assert(!left_down());

    // Hints back: quiet again.
    hint_on = true;
    news();
    run(30);
    before = rpc_calls;
    run(500);
    assert(rpc_calls - before <= 3);

    // Update activity never starts a split transaction.
    busy = true;
    news();
    before = rpc_calls;
    run(50);
    assert(rpc_calls == before);
    busy = false;

    puts("remote pad gating: news-only counter on the other half; fetch on news, heartbeat when quiet, backup "
         "while down, prompt lifts, dead link lifts, idle link loss harmless, older halves polled as before");
    return 0;
}
