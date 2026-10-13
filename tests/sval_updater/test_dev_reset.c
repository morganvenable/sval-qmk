// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

// Host test for the dev reset record (keyboards/svalboard/dev_reset.c): over any
// sequence of flashes, a build with a new token resets stored settings exactly
// once, and builds without a token never do.

#include <stdio.h>
#include <string.h>
#include "dev_reset.h"

static int checks, failures;
#define CHECK(c)                                                    \
    do {                                                            \
        checks++;                                                   \
        if (!(c)) {                                                 \
            failures++;                                             \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
        }                                                           \
    } while (0)

static dev_reset_record_t sector; // the dev reset sector
static int                resets;

static void erase(void) {
    memset(&sector, 0xFF, sizeof(sector));
}

// One boot of a build: token 0 is a build without one. As dev_reset.c: a reset
// when the record does not hold the token, then the token is recorded.
static void boot(uint32_t token) {
    if (!token) return;
    if (!dev_reset_done_for(&sector, token)) {
        resets++;
        dev_reset_record_make(&sector, token);
    }
}

// Flash a build, then boot it n times; returns the resets this caused.
static int flash(uint32_t token, int n) {
    int before = resets;
    while (n--) boot(token);
    return resets - before;
}

int main(void) {
    CHECK(!dev_reset_done_for(NULL, 1));
    erase();
    CHECK(!dev_reset_done_for(&sector, 0xFFFFFFFFu)); // an erased sector holds no token
    CHECK(flash(0, 3) == 0);                          // keep builds never reset
    CHECK(flash(0x1111, 3) == 1);                     // claussen: once, not on later boots
    CHECK(flash(0, 2) == 0);                          // keep afterwards: kept
    CHECK(flash(0x1111, 2) == 0);                     // the same build again: no second reset
    CHECK(flash(0x2222, 2) == 1);                     // blank, a new token: once
    CHECK(flash(0, 1) == 0);
    CHECK(flash(0x3333, 1) == 1);                     // claussen again, a new token: once
    CHECK(flash(0x2222, 1) == 1);                     // any token other than the last one: once
    CHECK(flash(0, 1) == 0);

    // a damaged record never counts as done
    dev_reset_record_make(&sector, 0x4444);
    CHECK(dev_reset_done_for(&sector, 0x4444));
    sector.check ^= 1;
    CHECK(!dev_reset_done_for(&sector, 0x4444));
    dev_reset_record_make(&sector, 0x4444);
    sector.magic ^= 0x100;
    CHECK(!dev_reset_done_for(&sector, 0x4444));
    erase();
    CHECK(flash(0x4444, 2) == 1); // a settings reset elsewhere never erases the record: only this

    printf("dev reset: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
