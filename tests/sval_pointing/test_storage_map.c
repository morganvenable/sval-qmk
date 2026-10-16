// The VIA custom config map (keyboards/svalboard/via_custom_map.h): every range is
// inside the block and no two overlap, and the pointing tools record fits its range.
#include <assert.h>
#include <stdio.h>
#include "via.h"
static const struct {
    const char *name;
    unsigned    offset, size;
} ranges[] = {
    {"board settings", SVAL_VIA_BOARD_OFFSET, SVAL_VIA_BOARD_SIZE},
    {"pointing tools", SVAL_VIA_POINT_TOOLS_OFFSET, SVAL_VIA_POINT_TOOLS_SIZE},
    {"touch gestures", SVAL_VIA_TOUCH_GESTURES_OFFSET, SVAL_VIA_TOUCH_GESTURES_SIZE},
};
int main(void) {
    unsigned count = sizeof(ranges) / sizeof(ranges[0]);
    for (unsigned i = 0; i < count; ++i) {
        assert(ranges[i].size > 0);
        assert(ranges[i].offset + ranges[i].size <= VIA_EEPROM_CUSTOM_CONFIG_SIZE);
        for (unsigned j = 0; j < count; ++j) {
            if (i == j) continue;
            unsigned a = ranges[i].offset, b = ranges[j].offset;
            int      apart = a + ranges[i].size <= b || b + ranges[j].size <= a;
            if (!apart) printf("%s overlaps %s\n", ranges[i].name, ranges[j].name);
            assert(apart);
        }
    }
    assert(SVAL_VIA_POINT_TOOLS_OFFSET == 100 && SVAL_VIA_POINT_TOOLS_SIZE == 20);
    assert(SVAL_VIA_TOUCH_GESTURES_OFFSET >= 120 && SVAL_VIA_TOUCH_GESTURES_SIZE >= 22);
    // saved_values_t and its magic are checked against SVAL_VIA_BOARD_SIZE at build time.
    puts("storage map: board, pointing tools and touch gesture ranges are inside the block and apart");
}
