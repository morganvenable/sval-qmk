// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// =====================================================================
//  DRY RUN release keys. NOT the production keys.
// =====================================================================
//
// The public halves of the two release signing keys, key_id 1 and key_id 2
// (docs/updater.md, "Release signing"). Every updater build compiles them in
// (updater/update_keys.c); release CI signs .svup files with the private half
// of one of them, held only in the release-signing environment's secret
// (D29).
//
// These two are throwaway DRY RUN keys made for M3 (D30): their private
// halves live on one development machine, outside the repository. While
// SVAL_UPDATE_RELEASE_KEYS_DRY_RUN is 1, release CI refuses every tag that is
// not a dry-run tag, and INFO reports the keys as DRY RUN (flags bit6).
//
// Swapping in the production keys is a change to this file only:
//   1. make_update.py --genkey for each new key, on the signing machine;
//   2. paste the two public keys below (make_update.py --print-public --key K);
//   3. set SVAL_UPDATE_RELEASE_KEYS_DRY_RUN to 0 and drop the DRY RUN banner;
//   4. put the seed of the key CI signs with in the release-signing secret.
// make_update.py and tools/check_release_elf.py read the keys from here, so
// nothing else changes. Both DRY RUN keys are also listed in
// tools/retired_release_keys.txt (leave them there): once step 3 sets the
// flag to 0, every release tool refuses this file if either slot still holds
// one of them, so a half-done swap cannot be released.

#define SVAL_UPDATE_RELEASE_KEYS_DRY_RUN 1

// key_id 1 (DRY RUN)
#define SVAL_UPDATE_RELEASE_KEY_1                                                                   \
    {                                                                                               \
        0x18, 0x86, 0x01, 0xeb, 0xe4, 0xb2, 0x8c, 0x7c, 0x0d, 0xc4, 0x3f, 0xec, 0xcd, 0xb5, 0x52, 0x67, \
        0xfc, 0xe0, 0x19, 0x89, 0xf8, 0xe7, 0xba, 0xb9, 0xc8, 0xfe, 0x16, 0x20, 0xfa, 0xc3, 0x90, 0x1f, \
    }

// key_id 2 (DRY RUN)
#define SVAL_UPDATE_RELEASE_KEY_2                                                                   \
    {                                                                                               \
        0xfb, 0x34, 0xf6, 0x55, 0x3b, 0xa6, 0x8c, 0x6b, 0x5d, 0x1a, 0xda, 0xe9, 0xaa, 0x7c, 0xa3, 0x3d, \
        0x0a, 0x7d, 0x9d, 0xc3, 0x72, 0x29, 0xdf, 0x99, 0x25, 0x8d, 0x20, 0xdc, 0x00, 0x01, 0xf7, 0x6b, \
    }
