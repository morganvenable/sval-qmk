// Raw HID framing (tmk_core/protocol/chibios/usb_packet.h) against the real
// ChibiOS input buffers queue: a 32-byte packet from a host sized for 32-byte
// reports, then a 64-byte one, on a 64-byte endpoint.
// Built and run by util/updater_test/run.sh.
#include <stdio.h>
#include <string.h>

#include "hal.h"
#include "usb_packet.h"

#define EP 64
#define NBUF 4

static uint8_t      storage[BQ_BUFFER_SIZE(NBUF, EP)];
static int          checks, failures;
static unsigned int released;

#define CHECK(c, ...)                         \
    do {                                      \
        checks++;                             \
        if (!(c)) {                           \
            failures++;                       \
            printf("FAIL %d: ", __LINE__);    \
            printf(__VA_ARGS__);              \
            printf("\n");                     \
        }                                     \
    } while (0)

static void on_release(io_buffers_queue_t *q) {
    (void)q;
    released++;
}

static void post(input_buffers_queue_t *q, uint8_t fill, size_t n) {
    uint8_t *b = ibqGetEmptyBufferI(q);
    memset(b, fill, n);
    b[0] = 0xDD; // looks like a wrapped request
    ibqPostFullBufferI(q, n);
}

static void setup(input_buffers_queue_t *q) {
    ibqObjectInit(q, false, storage, EP, NBUF, on_release, NULL);
    released = 0;
}

int main(void) {
    input_buffers_queue_t q;
    uint8_t               buf[EP];
    size_t                got;

    // The old byte-stream read joins the two packets: the misframing this fixes.
    setup(&q);
    post(&q, 0x11, 32);
    post(&q, 0x22, 64);
    CHECK(ibqReadTimeout(&q, buf, EP, TIME_IMMEDIATE) == EP, "byte read fills one report");
    CHECK(buf[31] == 0x11 && buf[32] == 0xDD, "byte read joins packets (expected, shows the bug)");

    // One packet per take: the short one comes out alone, then the full one intact.
    setup(&q);
    post(&q, 0x11, 32);
    post(&q, 0x22, 64);
    memset(buf, 0xEE, sizeof buf);
    CHECK(ibq_take_packet_s(&q, buf, EP, &got), "first packet taken");
    CHECK(got == 32, "short packet keeps its length: %zu", got);
    CHECK(buf[0] == 0xDD && buf[31] == 0x11, "short packet copied");
    int padded = 1;
    for (int i = 32; i < EP; i++) padded &= buf[i] == 0;
    CHECK(padded, "the rest of the report is zeroed");
    CHECK(released == 1, "short packet's buffer freed: %u", released);

    memset(buf, 0xEE, sizeof buf);
    CHECK(ibq_take_packet_s(&q, buf, EP, &got), "second packet taken");
    CHECK(got == EP, "full packet length: %zu", got);
    int whole = buf[0] == 0xDD;
    for (int i = 1; i < EP; i++) whole &= buf[i] == 0x22;
    CHECK(whole, "full packet framed from its first byte");
    CHECK(!ibq_take_packet_s(&q, buf, EP, &got), "queue empty afterwards");
    CHECK(released == 2, "both buffers freed: %u", released);

    // What raw_hid_task passes on: exactly one request, the 64-byte one.
    setup(&q);
    post(&q, 0x11, 32);
    post(&q, 0x22, 64);
    post(&q, 0x33, 32);
    int requests = 0, good = 0;
    while (ibq_take_packet_s(&q, buf, EP, &got)) {
        if (got != EP) continue;
        requests++;
        good += buf[0] == 0xDD && buf[1] == 0x22 && buf[EP - 1] == 0x22;
    }
    CHECK(requests == 1 && good == 1, "one correct request: %d requests, %d good", requests, good);

    // A packet longer than the caller's buffer is truncated, never overruns it.
    setup(&q);
    post(&q, 0x44, 64);
    uint8_t small[33];
    small[32] = 0x5A;
    CHECK(ibq_take_packet_s(&q, small, 32, &got) && got == 64, "long packet reports its length");
    CHECK(small[31] == 0x44 && small[32] == 0x5A, "copy stops at the caller's size");

    printf("raw HID framing: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
