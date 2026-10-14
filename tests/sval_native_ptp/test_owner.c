// Host test: multitouch.c and ptp.c together with SVAL_NATIVE_PTP. The owner
// command, lease refusal, local and remote frames reaching the touchpad
// without any host client, remote staleness and the mouse suppression hooks.
#define SVAL_MT_HOST_TEST
#define SVAL_MT_SPLIT
#define SVAL_NATIVE_PTP
#define SVAL_TOUCH_OWNER_DEFAULT 3
#define SVAL_MT_DEFAULT_PERIOD_MS 8
#define SVAL_PTP_HOST_TEST
#include "../../keyboards/svalboard/azoteq/multitouch.c" // brings mt_test_platform.h
#include "../../keyboards/svalboard/azoteq/ptp.c"
uint32_t sval_mt_rp_abort_source;

static void advance(uint32_t ms) {
    clock_ms += ms;
    ptp_test_us += ms * 1000;
    sval_mt_task();
}

static uint8_t owner_cmd(uint8_t action, uint8_t wanted, uint8_t *reply) {
    uint8_t cmd[64] = {0};
    put32(cmd + 1, 77);
    cmd[5] = 0xFC;
    cmd[6] = 0x42;
    cmd[7] = action;
    cmd[8] = wanted;
    sval_mt_command(cmd, 64);
    assert(cmd[6] == 0xC2);
    if (reply) memcpy(reply, cmd, 64);
    return cmd[7];
}

static uint8_t lease(void) {
    uint8_t cmd[64] = {0};
    put32(cmd + 1, 123);
    cmd[6] = 1;
    cmd[7] = 2; // pair capture, as unified.py asks
    sval_mt_command(cmd, 64);
    return cmd[7];
}

// One local IQS5xx sample: zero or one finger.
static void local_sample(bool finger, uint16_t x, uint16_t y) {
    uint8_t raw[45] = {0};
    raw[0] = 8;
    if (finger) {
        raw[5]  = 1;
        raw[10] = x >> 8;
        raw[11] = x & 255;
        raw[12] = y >> 8;
        raw[13] = y & 255;
        raw[15] = 40; // strength
        raw[16] = 20; // area
    }
    sval_mt_sample(raw, true);
}

// The other half's snapshot as the split RPC returns it (pad 0, the left).
static uint16_t remote_seq;
static void remote_snapshot_set(bool finger, uint16_t x, uint16_t y) {
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

int main(void) {
    uint8_t r[64];
    master    = true;
    connected = true;
    clock_ms  = 1000;
    sval_mt_init();
    assert(sval_mt_poll_ms() == 8);

    // Default owner NATIVE: the host-injection lease is refused.
    assert(owner_cmd(0, 0, r) == 0 && r[8] == SVAL_TOUCH_OWNER_NATIVE && (r[9] & 1) && r[10] == 1);
    assert(lease() == 5 && !client);
    assert(owner_cmd(1, SVAL_TOUCH_OWNER_DETECTOR, r) == 2 && r[8] == SVAL_TOUCH_OWNER_NATIVE);
    assert(owner_cmd(1, 9, r) == 2 && r[8] == SVAL_TOUCH_OWNER_NATIVE);
    assert(owner_cmd(2, 0, r) == 2);

    // Mouse until mode 3; then local and remote frames reach the touchpad
    // with no host client at all.
    local_sample(true, 100, 100);
    assert(!sval_mt_suppress_mouse() && !sval_mt_suppress_pair_mouse());
    uint8_t mode3[2] = {5, 3};
    assert(sval_ptp_set_report(3, 5, mode3, 2));
    assert(sval_mt_suppress_mouse() && sval_mt_suppress_pair_mouse());
    advance(5); // output on: both pads must show all-up again
    remote_snapshot_set(false, 0, 0);
    advance(5);                // the remote all-up arms the left pad
    local_sample(false, 0, 0); // arms the right (local) pad
    assert(pads[0].armed && pads[1].armed);
    local_sample(true, 300, 400);
    remote_snapshot_set(true, 50, 60);
    unsigned before = ptp_sent_count;
    advance(5);
    assert(ptp_sent_count == before + 1);
    const uint8_t *rep = ptp_sent[ptp_sent_count - 1];
    assert(rep[33] == 2);
    bool left_seen = false, right_seen = false;
    for (int i = 0; i < 2; i++) {
        uint16_t x = ptp_get16(rep + 3 + 6 * i), y = ptp_get16(rep + 5 + 6 * i);
        left_seen |= x == 50 && y == 60;
        right_seen |= x == 300 + SVAL_PTP_PAD_STRIDE && y == 400;
    }
    assert(left_seen && right_seen && !client && !count); // nothing queued for a host

    // The other half goes silent: only its contact lifts.
    connected = false;
    advance(5);
    advance(SVAL_PTP_REMOTE_STALE_MS);
    rep = ptp_sent[ptp_sent_count - 1];
    assert(rep[33] == 2);
    for (int i = 0; i < 2; i++) {
        uint16_t x = ptp_get16(rep + 3 + 6 * i);
        assert((rep[1 + 6 * i] & 2) == (x > SVAL_PTP_PAD_STRIDE ? 2 : 0));
    }
    local_sample(true, 301, 400);
    advance(5);
    rep = ptp_sent[ptp_sent_count - 1];
    assert(rep[33] == 1 && (rep[1] & 2)); // the local pad keeps reporting

    // Owner HOST: the touchpad releases; the lease is now accepted.
    assert(owner_cmd(1, SVAL_TOUCH_OWNER_HOST, r) == 0 && r[8] == SVAL_TOUCH_OWNER_HOST);
    before = ptp_sent_count;
    advance(5);
    assert(ptp_sent_count == before + 1 && ptp_sent[before][33] == 1 && !(ptp_sent[before][1] & 2));
    assert(sval_mt_suppress_mouse()); // held until every finger is up
    local_sample(false, 0, 0);
    advance(5);
    assert(!sval_mt_suppress_mouse());
    assert(lease() == 0 && client == 123);

    // Back to NATIVE while a host holds the lease: the lease ends first.
    assert(owner_cmd(1, SVAL_TOUCH_OWNER_NATIVE, r) == 0 && !client && r[10] == 1);
    assert(sval_mt_suppress_mouse()); // still Input Mode 3
    // Owner MOUSE: no lease, and the pad mouse is back once fingers are up.
    assert(owner_cmd(1, SVAL_TOUCH_OWNER_MOUSE, r) == 0 && r[8] == SVAL_TOUCH_OWNER_MOUSE);
    advance(5);
    assert(!sval_mt_suppress_mouse() && lease() == 5);
    puts("native PTP owner: default NATIVE, lease refused unless HOST, local and remote frames "
         "without a host client, stale remote lifts only its pad, handovers release first");
    return 0;
}
