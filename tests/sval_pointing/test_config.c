#include "sval.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "point_config.h"
#include "point_device.h"
#include "via.h"
extern uint8_t  test_storage[VIA_EEPROM_CUSTOM_CONFIG_SIZE];
extern uint32_t test_write_first, test_write_end;
#define AT SVAL_VIA_POINT_TOOLS_OFFSET
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
    assert(sval_point_command(p, 27));
    assert(p[2] == 0 && p[3] == 6);
    // Version 6 appends the touchpad and TrackPoint gains: 2.0 each by default.
    assert(p[25] == 32 && p[26] == 32);
    const uint8_t golden[16] = {36, 57, 73, 14, 112, 144, 228, 0, 7, 73, 14, 112, 144, 228, 0, 7};
    assert(!memcmp(p + 6, golden, 16));
    for (int i = 1; i <= 4; ++i)
        assert(sval_point_profile(i)->distance == 64 && sval_point_profile(i)->vertical_distance == 64);
    make_set(p);
    bits(p + 5, 6, 6, 45);
    bits(p + 5, 12, 1, 0); // shared horizontal64, vertical32, unlinked
    sval_point_command(p, 27);
    assert(!p[2]);
    sval_point_config_reload();
    for (int i = 1; i <= 4; ++i)
        assert(sval_point_profile(i)->distance == 64 && sval_point_profile(i)->vertical_distance == 32);
    p[1] = sval_cmd_point_tools_get;
    sval_point_command(p, 27);
    make_set(p);
    bits(p + 5, 13, 1, 0); // individual overrides enabled
    bits(p + 5, 14, 6, 54);
    bits(p + 5, 20, 6, 54);
    bits(p + 5, 27, 1, 0); // volume own linked16-count speed
    sval_point_command(p, 27);
    assert(!p[2]);
    assert(sval_point_profile(1)->vertical_distance == 16);
    assert(sval_point_profile(2)->vertical_distance == 32);
    make_set(p);
    p[3]--;
    sval_point_command(p, 27);
    assert(p[2] == 2);
    make_set(p);
    active = true;
    sval_point_command(p, 27);
    assert(p[2] == 1);
    active = false;
    make_set(p);
    bits(p + 5, 20, 6, 1);
    sval_point_command(p, 27);
    assert(p[2] == 3); // inconsistent linked pair
    make_set(p);
    bits(p + 5, 13, 1, 1);
    test_write_failed = true;
    sval_point_command(p, 27);
    assert(p[2] == 4);
    test_write_failed = false;
    assert(sval_point_profile(1)->distance == 16);
    test_storage[AT + 5] ^= 1;
    sval_point_config_reload();
    assert(sval_point_profile(1)->distance == 64);
    // Records from the earlier test builds (versions 1 and 2) are not read: nothing at
    // this address was ever written by them, and every new build resets settings anyway.
    uint8_t  old[20] = {0x50, 2, 36, 57, 73, 14, 117, 144, 228, 80, 7, 73, 14, 117, 144, 228, 160, 7};
    uint16_t sum     = crc(old);
    old[18]          = sum;
    old[19]          = sum >> 8;
    memcpy(test_storage + AT, old, 20);
    sval_point_config_reload();
    assert(sval_point_profile(1)->interval_ms == 0 && sval_point_profile(4)->interval_ms == 0);
    assert(test_storage[AT + 1] == 2); // no write on boot
    // v4 options round-trip independently, keeping a v3 speed record for rollback.
    p[1] = sval_cmd_point_tools_get;
    sval_point_command(p, 27);
    make_set(p);
    p[21] = (p[21] & 0xE0) | 4 | 16;
    sval_point_command(p, 27);
    assert(!p[2]);
    sval_point_config_reload();
    assert(sval_point_tool(1) == SVAL_TOOL_ZOOM && sval_point_inverted(1, 1));
    assert(test_storage[AT + 1] == 3);
    assert(crc(test_storage + AT) == (uint16_t)(test_storage[AT + 18] | test_storage[AT + 19] << 8));
    uint32_t intact = test_user_word;
    for (int bit = 0; bit < 32; ++bit) {
        test_user_word = intact ^ (1u << bit);
        sval_point_config_reload();
        assert(sval_point_tool(1) == SVAL_TOOL_VOLUME);
        assert(sval_point_profile(4)->interval_ms == 0);
    }
    test_user_word = intact;
    sval_point_config_reload();
    p[1] = sval_cmd_point_tools_get;
    sval_point_command(p, 27);
    make_set(p);
    p[21] |= 7;
    sval_point_command(p, 27);
    assert(p[2] == 0); // proportional preset fills the final 3-bit ID
    p[1] = sval_cmd_point_tools_get;
    sval_point_command(p, 27);
    make_set(p);
    p[23] |= 0x80;
    sval_point_command(p, 27);
    assert(p[2] == 3); // reserved bits
    p[1] = sval_cmd_point_tools_get;
    sval_point_command(p, 27);
    make_set(p);
    sval_point_command(p, 23);
    assert(p[2] == 3); // truncated request
    // Every write stayed inside the pointing tools' own ranges (via_custom_map.h): the
    // speed record and the gains record. Nothing else in the block changed.
    assert(test_write_first >= SVAL_VIA_POINT_TOOLS_OFFSET && test_write_end <= SVAL_VIA_POINT_GAINS_OFFSET + SVAL_VIA_POINT_GAINS_SIZE);
    for (int i = 0; i < VIA_EEPROM_CUSTOM_CONFIG_SIZE; ++i)
        if ((i < AT || i >= AT + 20) && (i < SVAL_VIA_POINT_GAINS_OFFSET || i >= SVAL_VIA_POINT_GAINS_OFFSET + 8)) assert(test_storage[i] == 0xA3);
    puts("config: axis pairs, inheritance, overrides, persistence, own storage range, CRC, stale/busy/invalid/failure guards passed");

    // Version 6: the device gains (touchpad, TrackPoint) after the tools.
    active = false;
    p[1]   = sval_cmd_point_tools_get;
    sval_point_command(p, 27);
    assert(p[3] == 6 && p[25] == 32 && p[26] == 32);
    assert(sval_point_gain(SVAL_POINT_DEVICE_PAD) == 32 && sval_point_gain(SVAL_POINT_DEVICE_STICK) == 32 && sval_point_gain(SVAL_POINT_DEVICE_BALL) == 16);
    uint8_t speeds[16], tools[3];
    memcpy(speeds, p + 6, 16);
    memcpy(tools, p + 22, 3);
    make_set(p);
    p[2]  = 6;
    p[24] = 48; // touchpad 3.0
    p[25] = 8;  // TrackPoint 0.5
    sval_point_command(p, 27);
    assert(p[2] == 0 && p[3] == 6 && p[25] == 48 && p[26] == 8);
    assert(!memcmp(p + 6, speeds, 16) && !memcmp(p + 22, tools, 3)); // the rest unchanged
    sval_point_config_reload();
    assert(sval_point_gain(SVAL_POINT_DEVICE_PAD) == 48 && sval_point_gain(SVAL_POINT_DEVICE_STICK) == 8);
    // A version 5 SET leaves the gains alone.
    p[1] = sval_cmd_point_tools_get;
    sval_point_command(p, 27);
    make_set(p);
    sval_point_command(p, 27);
    assert(p[2] == 0 && p[25] == 48 && p[26] == 8);
    // A zero gain, a short version 6 request or an unknown version is refused, unsaved.
    p[1] = sval_cmd_point_tools_get;
    sval_point_command(p, 27);
    make_set(p);
    p[2]  = 6;
    p[24] = 0;
    p[25] = 16;
    sval_point_command(p, 27);
    assert(p[2] == 3 && p[25] == 48);
    p[1] = sval_cmd_point_tools_get;
    sval_point_command(p, 27);
    make_set(p);
    p[2]  = 6;
    p[24] = 16;
    p[25] = 16;
    sval_point_command(p, 25);
    assert(p[2] == 3);
    p[1] = sval_cmd_point_tools_get;
    sval_point_command(p, 27);
    make_set(p);
    p[2] = 7;
    sval_point_command(p, 27);
    assert(p[2] == 3);
    // A damaged gains record reads as the defaults; the speed record is unaffected.
    test_storage[SVAL_VIA_POINT_GAINS_OFFSET + 2] ^= 1;
    sval_point_config_reload();
    assert(sval_point_gain(SVAL_POINT_DEVICE_PAD) == 32 && sval_point_gain(SVAL_POINT_DEVICE_STICK) == 32);
    p[1] = sval_cmd_point_tools_get;
    sval_point_command(p, 27);
    assert(!memcmp(p + 6, speeds, 16));
    // A save refused while a tool is active writes neither record.
    make_set(p);
    p[2]   = 6;
    p[24]  = 20;
    p[25]  = 20;
    active = true;
    sval_point_command(p, 27);
    assert(p[2] == 1 && p[25] == 32);
    active = false;
    puts("config: version 6 gains read, saved, kept by a version 5 save, validated, damaged record defaults passed");
}
