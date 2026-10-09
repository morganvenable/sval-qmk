// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

// Host test for a RELEASE updater build's key set (M3): built by
// util/updater_test/run.sh with SVAL_UPDATE_RELEASE and without
// SVAL_UPDATE_TEST_KEY, against the real update_keys.c and update_image.c.
//
//   test_release TEST_KEY_FILE [FILE.svup:ok|FILE.svup:refused ...]
//
// Checks that the build has the two release keys and no test key, that
// anything signed with the TEST-ONLY key is refused whatever key_id it names,
// and then runs each .svup through the checks a release build makes before
// it erases anything (signature, manifest fields, hash, image structure),
// expecting it to pass ("ok") or fail ("refused"). The .svup files come from
// outside (run.sh's optional SVAL_RELEASE_SVUPS, or a CI or local signing run).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "update_manifest.h"
#include "update_image.h"
#include "update_keys.h"
#include "update_release_keys.h"
#include "optional/monocypher-ed25519.h"

#ifndef SVAL_UPDATE_RELEASE
#    error "test_release.c is built as a release updater build"
#endif
#ifdef SVAL_UPDATE_TEST_KEY
#    error "test_release.c is built without the test key"
#endif

static int checks, failures;

#define CHECK(cond)                                                                   \
    do {                                                                              \
        checks++;                                                                     \
        if (!(cond)) {                                                                \
            failures++;                                                               \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                             \
    } while (0)

static uint8_t *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(n > 0 ? (size_t)n : 1);
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    *len = (size_t)n;
    return buf;
}

// The seed line of a make_update.py key file.
static int read_seed(const char *path, uint8_t seed[32]) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[256];
    int  ok = -1;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || strlen(line) < 64) continue;
        for (int i = 0; i < 32; i++) {
            unsigned v;
            if (sscanf(line + 2 * i, "%2x", &v) != 1) break;
            seed[i] = (uint8_t)v;
            if (i == 31) ok = 0;
        }
        break;
    }
    fclose(f);
    return ok;
}

// The checks a release build runs before it erases anything: ARM's manifest
// check, VERIFYING_MANIFEST's signature, VERIFYING_IMAGE's hash and structure.
static update_status_t release_check(const uint8_t *buf, size_t len) {
    if (len < UPDATE_SIGNED_MANIFEST_BYTES) return UPDATE_INVALID;
    sval_update_manifest_t m;
    memcpy(&m, buf, sizeof(m));
    const uint8_t  *image = buf + UPDATE_SIGNED_MANIFEST_BYTES;
    size_t          ilen  = len - UPDATE_SIGNED_MANIFEST_BYTES;
    update_device_t dev   = {
          .hand           = m.hand,
          .pointing_id    = m.pointing_id,
          .security_epoch = SVAL_UPDATE_SECURITY_EPOCH,
          .storage_format = SVAL_UPDATE_STORAGE_FORMAT,
          .release_build  = true,
          .max_image      = SVAL_UPDATE_MAX_IMAGE,
    };
    update_status_t st = update_manifest_check(&m, &dev);
    if (st != UPDATE_OK) return st;
    st = update_manifest_signature(buf);
    if (st != UPDATE_OK) return st;
    if (m.image_len != ilen) return UPDATE_BAD_HASH;
    uint8_t h[64];
    crypto_sha512(h, image, ilen);
    if (memcmp(h, m.sha512, 64) != 0) return UPDATE_BAD_HASH;
    return ilen >= UPDATE_HEAD_BYTES ? update_image_head_check(image, m.image_len) : UPDATE_BAD_IMAGE;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: test_release TEST_KEY_FILE [FILE.svup:ok|refused ...]\n");
        return 2;
    }

    // ---- the key set -------------------------------------------------------------------
    static const uint8_t release_pub[2][UPDATE_PUBKEY_BYTES] = {SVAL_UPDATE_RELEASE_KEY_1, SVAL_UPDATE_RELEASE_KEY_2};
    CHECK(update_key(UPDATE_KEY_TEST) == NULL);
    CHECK(update_key(UPDATE_KEY_RELEASE_1) && memcmp(update_key(UPDATE_KEY_RELEASE_1), release_pub[0], 32) == 0);
    CHECK(update_key(UPDATE_KEY_RELEASE_2) && memcmp(update_key(UPDATE_KEY_RELEASE_2), release_pub[1], 32) == 0);
    CHECK(update_key(3) == NULL);
    CHECK(memcmp(sval_update_build_info.magic, "SVBI", 4) == 0);
    CHECK(sval_update_build_info.flags & UPDATE_BUILD_RELEASE);
    CHECK(!(sval_update_build_info.flags & (UPDATE_BUILD_TEST_KEY | UPDATE_BUILD_TEST_HOOKS | UPDATE_BUILD_KEYTEST | UPDATE_BUILD_HOST_BOOTLOADER)));
    CHECK(sval_update_build_info.release_keys == 2);
    CHECK(!!(sval_update_build_info.flags & UPDATE_BUILD_KEYS_DRY_RUN) == !!SVAL_UPDATE_RELEASE_KEYS_DRY_RUN);

    // ---- the TEST-ONLY key signs nothing a release build takes -----------------------------
    uint8_t seed[32], sk[64], pk[32];
    CHECK(read_seed(argv[1], seed) == 0);
    crypto_ed25519_key_pair(sk, pk, seed); // wipes seed
    CHECK(memcmp(pk, release_pub[0], 32) != 0 && memcmp(pk, release_pub[1], 32) != 0);
    sval_update_manifest_t m;
    memset(&m, 0, sizeof(m));
    m.magic          = UPDATE_MANIFEST_MAGIC;
    m.manifest_ver   = UPDATE_MANIFEST_VERSION;
    m.hand           = UPDATE_HAND_RIGHT;
    m.pointing_id    = UPDATE_POINTING_PMW3389;
    m.keymap_id      = UPDATE_KEYMAP_SVAL;
    m.storage_format = SVAL_UPDATE_STORAGE_FORMAT;
    m.updater_proto  = UPDATE_PROTOCOL_VERSION;
    m.image_len      = 0x400;
    update_device_t dev = {.hand = UPDATE_HAND_RIGHT, .pointing_id = UPDATE_POINTING_PMW3389, .storage_format = SVAL_UPDATE_STORAGE_FORMAT, .release_build = true, .max_image = SVAL_UPDATE_MAX_IMAGE};
    for (uint8_t key_id = 0; key_id < UPDATE_KEY_COUNT; key_id++) {
        for (uint16_t flags = 0; flags <= UPDATE_FLAG_RELEASE; flags++) {
            uint8_t blob[UPDATE_SIGNED_MANIFEST_BYTES];
            m.key_id = key_id;
            m.flags  = flags;
            memcpy(blob, &m, sizeof(m));
            crypto_ed25519_sign(blob + UPDATE_MANIFEST_BYTES, sk, blob, UPDATE_MANIFEST_BYTES);
            CHECK(update_manifest_signature(blob) == UPDATE_BAD_SIG);
            if (key_id == UPDATE_KEY_TEST) CHECK(update_manifest_check(&m, &dev) == UPDATE_BAD_SIG);
        }
    }
    m.key_id = UPDATE_KEY_RELEASE_1;
    m.flags  = UPDATE_FLAG_RELEASE | UPDATE_FLAG_DIAGNOSTIC;
    CHECK(update_manifest_check(&m, &dev) == UPDATE_BAD_IMAGE); // a release build takes no DIAGNOSTIC image

    // ---- .svup files from outside ----------------------------------------------------------
    int files = 0;
    for (int i = 2; i < argc; i++) {
        char  path[4096];
        char *colon = strrchr(argv[i], ':');
        if (!colon || (strcmp(colon + 1, "ok") && strcmp(colon + 1, "refused"))) {
            fprintf(stderr, "%s: expected FILE.svup:ok or FILE.svup:refused\n", argv[i]);
            return 2;
        }
        snprintf(path, sizeof(path), "%.*s", (int)(colon - argv[i]), argv[i]);
        size_t   len;
        uint8_t *buf = read_file(path, &len);
        CHECK(buf != NULL);
        if (!buf) continue;
        update_status_t st   = release_check(buf, len);
        int             want = strcmp(colon + 1, "ok") == 0;
        printf("  %s: %s (status %d), expected %s\n", path, st == UPDATE_OK ? "accepted" : "refused", st, colon + 1);
        CHECK((st == UPDATE_OK) == want);
        files++;
        free(buf);
    }

    printf("release build key set: %d checks, %d failures, %d .svup files\n", checks, failures, files);
    return failures ? 1 : 0;
}
