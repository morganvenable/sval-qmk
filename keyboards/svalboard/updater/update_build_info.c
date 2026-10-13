// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

// The build-info record (see update_build_info.h), and the guards that keep
// test and diagnostic features out of release updater builds.

#include <stddef.h>
#include "update_build_info.h"
#include "update_self.h"
#include "updater.h" // SVAL_FW_VERSION

#if defined(SVAL_UPDATE_TEST_HOOKS) && defined(SVAL_UPDATE_RELEASE)
#    error "release updater builds cannot have test hooks"
#elif defined(SVAL_KEYTEST) && defined(SVAL_UPDATE_RELEASE)
#    error "release updater builds cannot have SVAL_KEYTEST"
#elif defined(SVAL_TEST_WIPE) && defined(SVAL_UPDATE_RELEASE)
#    error "release updater builds cannot have SVAL_TEST_WIPE (bench builds only)"
#elif defined(SVAL_UPDATE_NO_CHORD) && defined(SVAL_UPDATE_RELEASE)
#    error "release updater builds cannot have SVAL_UPDATE_NO_CHORD (dev builds only)"
#elif defined(SVAL_DEV_KEEP_SETTINGS) && defined(SVAL_UPDATE_RELEASE)
#    error "release updater builds cannot have SVAL_DEV_KEEP_SETTINGS (dev builds only)"
#endif
#if defined(SVAL_UPDATE_RELEASE) && defined(SVAL_HOST_BOOTLOADER)
#    if SVAL_HOST_BOOTLOADER
#        error "release updater builds cannot have SVAL_HOST_BOOTLOADER"
#    endif
#endif

#ifndef SVAL_FW_VERSION_STRING
#    define SVAL_FW_VERSION_STRING ""
#endif
_Static_assert(sizeof(SVAL_FW_VERSION_STRING) <= UPDATE_VERSION_CHARS + 1, "SVAL_FW_VERSION_STRING is longer than 16 characters");
_Static_assert(sizeof(update_build_info_t) == 32, "update_build_info_t is 32 bytes");
#ifndef SVAL_UPDATE_KEYMAP_ID
#    define SVAL_UPDATE_KEYMAP_ID 0
#endif
#ifdef SVAL_UPDATER_HOST_TEST
#    define BI_HAND 0xFF
#    define BI_POINTING 0xFF
#else
#    define BI_HAND UPDATE_SELF_HAND
#    define BI_POINTING UPDATE_SELF_POINTING
#endif

#if defined(SVAL_HOST_BOOTLOADER)
#    if SVAL_HOST_BOOTLOADER
#        define BI_HOST_BOOTLOADER UPDATE_BUILD_HOST_BOOTLOADER
#    endif
#endif
#ifndef BI_HOST_BOOTLOADER
#    define BI_HOST_BOOTLOADER 0
#endif

const update_build_info_t sval_update_build_info = {
    .magic = {'S', 'V', 'B', 'I'},
    .info_ver = UPDATE_BUILD_INFO_VERSION,
    .flags = 0
#ifdef SVAL_UPDATE_RELEASE
             | UPDATE_BUILD_RELEASE
#endif
#ifdef SVAL_UPDATE_TEST_HOOKS
             | UPDATE_BUILD_TEST_HOOKS
#endif
#ifdef SVAL_KEYTEST
             | UPDATE_BUILD_KEYTEST
#endif
#ifdef SVAL_UPDATE_NO_CHORD
             | UPDATE_BUILD_NO_CHORD
#endif
#ifdef SVAL_DEV_KEEP_SETTINGS
             | UPDATE_BUILD_DEV_KEEP
#endif
             | BI_HOST_BOOTLOADER,
    .reserved0 = 0,
    .updater_proto = UPDATE_PROTOCOL_VERSION,
    .fw_version = SVAL_FW_VERSION,
    .version = SVAL_FW_VERSION_STRING,
    .hand = BI_HAND,
    .pointing_id = BI_POINTING,
    .keymap_id = SVAL_UPDATE_KEYMAP_ID,
    .reserved = 0,
};
