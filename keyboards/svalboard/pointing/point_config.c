// SPDX-License-Identifier: GPL-2.0-or-later
#include "point_config.h"
#include "point_control.h"
#include "svalboard.h"
#include "via.h"
#include "sval.h"
#include "eeconfig.h"
_Static_assert(EECONFIG_USER_DATA_SIZE == 0, "Pointing tools require the unused user config word");
#include <string.h>

// Unused tail of the existing 100-byte VIA custom block. Never moves keymaps,
// macros or Sval tables, and never changes their layout stamps.
#define CONFIG_OFFSET 80
#define CONFIG_SIZE 20
_Static_assert(sizeof(saved_values_t) + 6 <= CONFIG_OFFSET, "Pointing settings overlap board settings/magic");
_Static_assert(CONFIG_OFFSET + CONFIG_SIZE <= VIA_EEPROM_CUSTOM_CONFIG_SIZE, "Pointing settings exceed reserved VIA block");
static sval_point_profile_t       profiles[SVAL_POINT_PROFILE_COUNT];
static bool                       loaded;
static uint16_t                   generation;
static const sval_point_profile_t defaults[] = {{64, 64, 0, 8}, {64, 64, 0, 8}, {64, 64, 0, 8}, {64, 64, 0, 8}};

static uint16_t crc(const uint8_t *data, uint8_t size) {
    uint16_t value = 0xFFFF;
    while (size--) {
        value ^= (uint16_t)*data++ << 8;
        for (uint8_t bit = 0; bit < 8; ++bit)
            value = (value & 0x8000) ? (value << 1) ^ 0x1021 : value << 1;
    }
    return value;
}
typedef struct {
    uint8_t h, v;
    bool    linked;
} speed_t;
static speed_t shared, speeds[4];
static bool    universal, inherit[4];
// Four configurable slots: 3-bit preset and two direction flags per slot.
// The existing user word is unused by this keymap; no EEPROM addresses move.
static uint8_t tools[4], inversions[4];
static uint8_t options_crc(uint32_t bits) {
    uint8_t c = 0;
    for (uint8_t n = 0; n < 3; ++n) {
        c ^= bits >> (n * 8);
        for (uint8_t i = 0; i < 8; ++i)
            c = c & 0x80 ? (c << 1) ^ 7 : c << 1;
    }
    return c;
}
static uint32_t options_word(const uint8_t *options) {
    uint32_t bits = 0xA00000u | options[0] | (uint32_t)options[1] << 8 | (uint32_t)options[2] << 16;
    return bits | (uint32_t)options_crc(bits) << 24;
}
static void encode_options(uint8_t *data) {
    uint32_t bits = 0;
    for (uint8_t i = 0; i < 4; ++i)
        bits |= (uint32_t)(tools[i] | inversions[i] << 3) << (i * 5);
    for (uint8_t i = 0; i < 3; ++i)
        data[i] = bits >> (8 * i);
}
static bool decode_options(const uint8_t *data, bool apply) {
    uint32_t bits = data[0] | (uint32_t)data[1] << 8 | (uint32_t)data[2] << 16;
    if (bits & 0xF00000u) return false;
    for (uint8_t i = 0; i < 4; ++i)
        if (((bits >> (i * 5)) & 7) >= SVAL_TOOL_COUNT) return false;
    if (apply)
        for (uint8_t i = 0; i < 4; ++i) {
            tools[i]      = (bits >> (i * 5)) & 7;
            inversions[i] = (bits >> (i * 5 + 3)) & 3;
        }
    return true;
}
static const uint16_t distances[64] = {1024, 948, 878, 813, 753, 697, 645, 597, 553, 512, 474, 439, 406, 376, 348, 323, 299, 276, 256, 237, 219, 203, 188, 174, 161, 149, 138, 128, 119, 110, 102, 94, 87, 81, 75, 69, 64, 59, 55, 51, 47, 44, 40, 37, 35, 32, 30, 27, 25, 24, 22, 20, 19, 17, 16, 15, 14, 13, 12, 11, 10, 9, 9, 8};
static uint8_t        nearest(uint16_t distance) {
           uint8_t  best = 0;
           uint16_t diff = 65535;
           for (uint8_t i = 0; i < 64; ++i) {
               uint16_t d = distances[i] > distance ? distances[i] - distance : distance - distances[i];
               if (d < diff) {
                   best = i;
                   diff = d;
        }
    }
           return best;
}
static uint16_t get_bits(const uint8_t *data, uint8_t *bit, uint8_t width) {
    uint16_t value = 0;
    for (uint8_t i = 0; i < width; ++i, ++*bit)
        value |= ((data[*bit / 8] >> (*bit % 8)) & 1) << i;
    return value;
}
static void put_bits(uint8_t *data, uint8_t *bit, uint16_t value, uint8_t width) {
    for (uint8_t i = 0; i < width; ++i, ++*bit)
        data[*bit / 8] |= ((value >> i) & 1) << (*bit % 8);
}
static speed_t read_speed(const uint8_t *data, uint8_t *bit) {
    speed_t s;
    s.h      = get_bits(data, bit, 6);
    s.v      = get_bits(data, bit, 6);
    s.linked = get_bits(data, bit, 1);
    return s;
}
static void write_speed(uint8_t *data, uint8_t *bit, speed_t s) {
    put_bits(data, bit, s.h, 6);
    put_bits(data, bit, s.v, 6);
    put_bits(data, bit, s.linked, 1);
}
static void resolve(void) {
    for (uint8_t i = 0; i < 4; ++i) {
        speed_t s                     = universal || inherit[i] ? shared : speeds[i];
        profiles[i].distance          = distances[s.h];
        profiles[i].vertical_distance = distances[s.v];
    }
}
static void encode(uint8_t *data) {
    memset(data, 0, 16);
    uint8_t bit = 0;
    write_speed(data, &bit, shared);
    put_bits(data, &bit, universal, 1);
    for (uint8_t i = 0; i < 4; ++i) {
        write_speed(data, &bit, speeds[i]);
        put_bits(data, &bit, inherit[i], 1);
        put_bits(data, &bit, profiles[i].interval_ms, 8);
        put_bits(data, &bit, profiles[i].reversal - 1, 6);
    }
}
// Validate the complete candidate before touching any active setting.
static bool decode(const uint8_t *data, bool apply) {
    uint8_t bit    = 0;
    speed_t common = read_speed(data, &bit);
    bool    all    = get_bits(data, &bit, 1);
    speed_t candidate[4];
    bool    use[4];
    uint8_t intervals[4], reversals[4];
    if (common.linked && common.h != common.v) return false;
    for (uint8_t i = 0; i < 4; ++i) {
        candidate[i] = read_speed(data, &bit);
        use[i]       = get_bits(data, &bit, 1);
        intervals[i] = get_bits(data, &bit, 8);
        reversals[i] = get_bits(data, &bit, 6) + 1;
        if ((candidate[i].linked && candidate[i].h != candidate[i].v) || (intervals[i] && intervals[i] < 4) || intervals[i] > 250) return false;
    }
    if (data[15] & 0xC0) return false;
    if (apply) {
        shared    = common;
        universal = all;
        for (uint8_t i = 0; i < 4; ++i) {
            speeds[i]               = candidate[i];
            inherit[i]              = use[i];
            profiles[i].interval_ms = intervals[i];
            profiles[i].reversal    = reversals[i];
        }
        resolve();
    }
    return true;
}
void sval_point_config_reload(void) {
    uint8_t stored[CONFIG_SIZE];
    memcpy(profiles, defaults, sizeof(profiles));
    shared    = (speed_t){36, 36, true};
    universal = true;
    for (uint8_t i = 0; i < 4; ++i) {
        speeds[i]     = shared;
        inherit[i]    = true;
        tools[i]      = i;
        inversions[i] = 0;
    }
    resolve();
    if (via_read_custom_config(stored, CONFIG_OFFSET, sizeof(stored)) == sizeof(stored) && stored[0] == 0x50 && crc(stored, 18) == (uint16_t)(stored[18] | stored[19] << 8)) {
        if (stored[1] == 2 || stored[1] == 3)
            decode(stored + 2, true);
        else if (stored[1] == 1) {
            bool valid = true;
            for (uint8_t i = 0; i < 4; ++i) {
                uint8_t *p = stored + 2 + i * 4;
                uint16_t d = p[0] | p[1] << 8;
                if (d < 8 || d > 1024 || p[2] < 40 || p[2] > 250 || !p[3] || p[3] > 64 || p[3] > d) valid = false;
            }
            if (valid) {
                universal = false;
                for (uint8_t i = 0; i < 4; ++i) {
                    uint8_t *p              = stored + 2 + i * 4;
                    uint8_t  code           = nearest(p[0] | p[1] << 8);
                    speeds[i]               = (speed_t){code, code, true};
                    inherit[i]              = code == 36;
                    profiles[i].interval_ms = p[2];
                    profiles[i].reversal    = p[3];
                }
                resolve();
            }
        }
    }
    uint32_t options  = eeconfig_read_user();
    uint8_t  extra[3] = {options, options >> 8, (options >> 16) & 15};
    if (((options >> 20) & 15) == 0xA && (options >> 24) == options_crc(options)) decode_options(extra, true);
    loaded = true;
    ++generation;
}
const sval_point_profile_t *sval_point_profile(uint8_t control) {
    if (!loaded) sval_point_config_reload();
    return &profiles[control >= 1 && control <= 4 ? control - 1 : 0];
}
uint8_t sval_point_tool(uint8_t control) {
    if (!loaded) sval_point_config_reload();
    return tools[control >= 1 && control <= 4 ? control - 1 : 0];
}
bool sval_point_inverted(uint8_t control, uint8_t axis) {
    if (!loaded) sval_point_config_reload();
    return (inversions[control >= 1 && control <= 4 ? control - 1 : 0] & (1u << axis)) != 0;
}
bool sval_point_command(uint8_t *data, uint8_t length) {
    if (length < 2 || data[1] < sval_cmd_point_tools_get || data[1] > sval_cmd_point_tools_stop) return false;
    if (!loaded) sval_point_config_reload();
    if (data[1] == sval_cmd_point_tools_status) {
        sval_point_status(data);
        return true;
    }
    if (data[1] == sval_cmd_point_tools_stop) {
        sval_point_stop();
        data[2] = 0;
        data[3] = 1;
        return true;
    }
    uint8_t status = 0;
    if (data[1] == sval_cmd_point_tools_set) {
        uint8_t candidate[19] = {0};
        if (length >= 24) memcpy(candidate, data + 5, 19);
        if (length < 24 || data[2] != SVAL_POINT_CONFIG_VERSION || !decode(candidate, false) || !decode_options(candidate + 16, false))
            status = 3;
        else if ((uint16_t)(data[3] | data[4] << 8) != generation)
            status = 2;
        else if (sval_point_active(0) || sval_point_active(1))
            status = 1;
        else {
            // Keep the speed record readable by the previous tuning branch.
            uint8_t stored[CONFIG_SIZE] = {0x50, 3}, check[CONFIG_SIZE];
            memcpy(stored + 2, candidate, 16);
            uint32_t options = options_word(candidate + 16);
            uint16_t sum     = crc(stored, 18);
            stored[18]       = sum;
            stored[19]       = sum >> 8;
            eeconfig_update_user(options);
            via_update_custom_config(stored, CONFIG_OFFSET, CONFIG_SIZE);
            via_read_custom_config(check, CONFIG_OFFSET, CONFIG_SIZE);
            if (memcmp(stored, check, sizeof(stored)) || eeconfig_read_user() != options || sval_storage_write_failed())
                status = 4;
            else {
                decode(candidate, true);
                decode_options(candidate + 16, true);
                ++generation;
                sval_point_tuning_changed();
            }
        }
    }
    data[2] = status;
    data[3] = SVAL_POINT_CONFIG_VERSION;
    data[4] = generation;
    data[5] = generation >> 8;
    encode(data + 6);
    encode_options(data + 22);
    return true;
}
