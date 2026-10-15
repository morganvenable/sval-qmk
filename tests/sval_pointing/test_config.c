#include "sval.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "point_config.h"
extern uint8_t  test_storage[100];
extern bool     test_write_failed;
extern uint32_t test_user_word;
static bool     active;
bool            sval_point_active(uint8_t side) {
               (void)side;
               return active;
}
void sval_point_status(uint8_t *data) {
    data[2] = 1;
}
void sval_point_stop(void) {
    active = false;
}
void        sval_point_tuning_changed(void) {}
static void bits(uint8_t *data, int start, int width, int value) {
    for (int i = 0; i < width; ++i) {
        int bit       = start + i;
        data[bit / 8] = (data[bit / 8] & ~(1 << (bit % 8))) | (((value >> i) & 1) << (bit % 8));
    }
}
static void make_set(uint8_t *data) {
    uint8_t table[19];
    memcpy(table, data + 6, 19);
    uint8_t lo = data[4], hi = data[5];
    memset(data, 0, 32);
    data[0] = 0xDF;
    data[1] = sval_cmd_point_tools_set;
    data[2] = 5;
    data[3] = lo;
    data[4] = hi;
    memcpy(data + 5, table, 19);
}
static uint16_t crc(const uint8_t *p) {
    uint16_t c = 0xffff;
    for (int i = 0; i < 18; ++i) {
        c ^= p[i] << 8;
        for (int j = 0; j < 8; ++j)
            c = c & 0x8000 ? (c << 1) ^ 0x1021 : c << 1;
    }
    return c;
}
int main(void) {
    memset(test_storage, 0xA3, sizeof(test_storage));
    uint8_t p[32] = {0xDF, sval_cmd_point_tools_get};
    assert(sval_point_command(p, 26));
    assert(p[2] == 0 && p[3] == 5);
    const uint8_t golden[16] = {36, 57, 73, 14, 112, 144, 228, 0, 7, 73, 14, 112, 144, 228, 0, 7};
    assert(!memcmp(p + 6, golden, 16));
    for (int i = 1; i <= 4; ++i)
        assert(sval_point_profile(i)->distance == 64 && sval_point_profile(i)->vertical_distance == 64);
    make_set(p);
    bits(p + 5, 6, 6, 45);
    bits(p + 5, 12, 1, 0); // shared horizontal64, vertical32, unlinked
    sval_point_command(p, 26);
    assert(!p[2]);
    sval_point_config_reload();
    for (int i = 1; i <= 4; ++i)
        assert(sval_point_profile(i)->distance == 64 && sval_point_profile(i)->vertical_distance == 32);
    p[1] = sval_cmd_point_tools_get;
    sval_point_command(p, 26);
    make_set(p);
    bits(p + 5, 13, 1, 0); // individual overrides enabled
    bits(p + 5, 14, 6, 54);
    bits(p + 5, 20, 6, 54);
    bits(p + 5, 27, 1, 0); // volume own linked16-count speed
    sval_point_command(p, 26);
    assert(!p[2]);
    assert(sval_point_profile(1)->vertical_distance == 16);
    assert(sval_point_profile(2)->vertical_distance == 32);
    make_set(p);
    p[3]--;
    sval_point_command(p, 26);
    assert(p[2] == 2);
    make_set(p);
    active = true;
    sval_point_command(p, 26);
    assert(p[2] == 1);
    active = false;
    make_set(p);
    bits(p + 5, 20, 6, 1);
    sval_point_command(p, 26);
    assert(p[2] == 3); // inconsistent linked pair
    make_set(p);
    bits(p + 5, 13, 1, 1);
    test_write_failed = true;
    sval_point_command(p, 26);
    assert(p[2] == 4);
    test_write_failed = false;
    assert(sval_point_profile(1)->distance == 16);
    test_storage[85] ^= 1;
    sval_point_config_reload();
    assert(sval_point_profile(1)->distance == 64);
    // Migrate the original persisted table without touching any other settings.
    uint8_t  old[20] = {0x50, 1, 64, 0, 80, 8, 32, 0, 80, 8, 96, 0, 80, 8, 96, 0, 160, 8};
    uint16_t sum     = crc(old);
    old[18]          = sum;
    old[19]          = sum >> 8;
    memcpy(test_storage + 80, old, 20);
    sval_point_config_reload();
    assert(sval_point_profile(2)->distance == 32);
    assert(sval_point_profile(4)->interval_ms == 160);
    assert(test_storage[81] == 1); // no write on boot
    const uint8_t v2[16] = {36, 57, 73, 14, 117, 144, 228, 80, 7, 73, 14, 117, 144, 228, 160, 7};
    old[0]               = 0x50;
    old[1]               = 2;
    memcpy(old + 2, v2, 16);
    sum     = crc(old);
    old[18] = sum;
    old[19] = sum >> 8;
    memcpy(test_storage + 80, old, 20);
    sval_point_config_reload();
    assert(sval_point_profile(1)->distance == 64 && sval_point_profile(1)->interval_ms == 80 && sval_point_profile(4)->interval_ms == 160 && test_storage[81] == 2);
    // v4 options round-trip independently, keeping a v3 speed record for rollback.
    p[1] = sval_cmd_point_tools_get;
    sval_point_command(p, 26);
    make_set(p);
    p[21] = (p[21] & 0xE0) | 4 | 16;
    sval_point_command(p, 26);
    assert(!p[2]);
    sval_point_config_reload();
    assert(sval_point_tool(1) == SVAL_TOOL_ZOOM && sval_point_inverted(1, 1));
    assert(test_storage[81] == 3);
    assert(crc(test_storage + 80) == (uint16_t)(test_storage[98] | test_storage[99] << 8));
    uint32_t intact = test_user_word;
    for (int bit = 0; bit < 32; ++bit) {
        test_user_word = intact ^ (1u << bit);
        sval_point_config_reload();
        assert(sval_point_tool(1) == SVAL_TOOL_VOLUME);
        assert(sval_point_profile(4)->interval_ms == 160);
    }
    test_user_word = intact;
    sval_point_config_reload();
    p[1] = sval_cmd_point_tools_get;
    sval_point_command(p, 26);
    make_set(p);
    p[21] |= 7;
    sval_point_command(p, 26);
    assert(p[2] == 0); // proportional preset fills the final 3-bit ID
    p[1] = sval_cmd_point_tools_get;
    sval_point_command(p, 26);
    make_set(p);
    p[23] |= 0x80;
    sval_point_command(p, 26);
    assert(p[2] == 3); // reserved bits
    p[1] = sval_cmd_point_tools_get;
    sval_point_command(p, 26);
    make_set(p);
    sval_point_command(p, 23);
    assert(p[2] == 3); // truncated request
    for (int i = 0; i < 80; ++i)
        assert(test_storage[i] == 0xA3);
    puts("config: axis pairs, inheritance, overrides, persistence, migration, CRC, stale/busy/invalid/failure guards passed");
}
