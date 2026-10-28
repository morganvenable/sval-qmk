// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

// The ID-writer's logic on a PC, for the station's dry runs and tests
// (tests/sval_board_id): a file-backed board.
//
//   writer_host DEVICE_DIR PARAMS.bin REPORT.bin
//
// DEVICE_DIR/block.bin  the 64 KiB record block (0xFF when blank)
// DEVICE_DIR/chip.bin   jedec capacity (1 byte), unique ID (8), then the strap's
//                       high counts under pull-up, pull-down, no pull (3)
// Programming follows NOR rules: it can only clear bits. Erasing is not possible.

#include "writer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t block[BOARD_RECORD_SIZE];
static uint8_t chip[12];
static int     programs;

uint8_t hal_jedec_capacity(void) {
    return chip[0];
}

void hal_unique_id(uint8_t id[BOARD_RECORD_UID_BYTES]) {
    memcpy(id, chip + 1, BOARD_RECORD_UID_BYTES);
}

void hal_strap_sample(uint8_t *pu_high, uint8_t *pd_high, uint8_t *np_high, uint8_t samples) {
    *pu_high = chip[9] > samples ? samples : chip[9];
    *pd_high = chip[10] > samples ? samples : chip[10];
    *np_high = chip[11] > samples ? samples : chip[11];
}

const uint8_t *hal_block(void) {
    return block;
}

void hal_program_page(uint16_t page, const uint8_t *data) {
    if (page >= BOARD_RECORD_PAGES) abort();
    for (uint16_t i = 0; i < BOARD_RECORD_PAGE; i++) block[(uint32_t)page * BOARD_RECORD_PAGE + i] &= data[i];
    programs++;
}

static int load(const char *path, void *buf, size_t n) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t got = fread(buf, 1, n, f);
    fclose(f);
    return got == n ? 0 : -1;
}

static int save(const char *path, const void *buf, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t put = fwrite(buf, 1, n, f);
    return fclose(f) == 0 && put == n ? 0 : -1;
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: writer_host DEVICE_DIR PARAMS.bin REPORT.bin\n");
        return 2;
    }
    char path[4096];
    snprintf(path, sizeof(path), "%s/block.bin", argv[1]);
    if (load(path, block, sizeof(block))) return fprintf(stderr, "no %s\n", path), 2;
    char chip_path[4096];
    snprintf(chip_path, sizeof(chip_path), "%s/chip.bin", argv[1]);
    if (load(chip_path, chip, sizeof(chip))) return fprintf(stderr, "no %s\n", chip_path), 2;
    static writer_params_t p;
    if (load(argv[2], &p, sizeof(p))) return fprintf(stderr, "bad params file\n"), 2;
    static writer_report_t r;
    writer_run(&p, &r);
    if (programs && save(path, block, sizeof(block))) return 2;
    return save(argv[3], &r, sizeof(r)) ? 2 : 0;
}
