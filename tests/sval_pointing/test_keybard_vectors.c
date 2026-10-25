// Keybard-encoded SET payloads (keybard_vectors.txt) through the real point_config.c: after
// saving and reloading, the firmware must resolve exactly the settings Keybard meant.
#include "sval.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "point_config.h"
bool sval_point_active(uint8_t side) {
    (void)side;
    return false;
}
void sval_point_status(uint8_t *data) {
    data[2] = 1;
}
void sval_point_stop(void) {}
void sval_point_tuning_changed(void) {}
int  main(int argc, char **argv) {
    FILE *f = argc > 1 ? fopen(argv[1], "r") : NULL;
    if (!f) return 2;
    char line[512];
    int  count = 0, failures = 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#') continue;
        char *expected = strchr(line, '|');
        if (!expected) return 2;
        *expected++                         = 0;
        expected[strcspn(expected, "\r\n")] = 0;
        uint8_t payload[19];
        char   *s = line;
        for (int n = 0; n < 19; ++n)
            payload[n] = strtoul(s, &s, 16);
        uint8_t d[64] = {0xDF, sval_cmd_point_tools_get};
        sval_point_command(d, 64);
        uint8_t lo = d[4], hi = d[5];
        memset(d, 0, sizeof d);
        d[0] = 0xDF;
        d[1] = sval_cmd_point_tools_set;
        d[2] = SVAL_POINT_CONFIG_VERSION_V5; // the vectors are version 5 payloads (no gains)
        d[3] = lo;
        d[4] = hi;
        memcpy(d + 5, payload, 19);
        sval_point_command(d, 64);
        char actual[256];
        int  at = snprintf(actual, sizeof actual, "%d", d[2]);
        sval_point_config_reload();
        for (int i = 1; i <= 4 && d[2] == 0; ++i) {
            const sval_point_profile_t *p = sval_point_profile(i);
            at += snprintf(actual + at, sizeof actual - at, " %d,%d,%d,%d,%d,%d,%d", p->distance, p->vertical_distance, p->interval_ms, p->reversal, sval_point_tool(i), sval_point_inverted(i, 0), sval_point_inverted(i, 1));
        }
        ++count;
        if (strcmp(actual, expected)) {
            printf("FAIL vector %d\n  want %s\n  got  %s\n", count, expected, actual);
            ++failures;
        }
    }
    fclose(f);
    if (!count || failures) return 1;
    printf("OK %d vectors\n", count);
    return 0;
}
