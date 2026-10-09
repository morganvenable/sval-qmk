// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// This build's side and pointing device (D18, R8): the image check compares
// builds, it does not detect hardware. update_image.c checks manifests against
// them, and update_keys.c records them in the build-info record so the release
// signer can cross-check a manifest against the image itself (M3).

#include "update_manifest.h"

#ifndef SVAL_UPDATER_HOST_TEST
#    if defined(INIT_EE_HANDS_LEFT) && !defined(INIT_EE_HANDS_RIGHT)
#        define UPDATE_SELF_HAND UPDATE_HAND_LEFT
#    elif defined(INIT_EE_HANDS_RIGHT) && !defined(INIT_EE_HANDS_LEFT)
#        define UPDATE_SELF_HAND UPDATE_HAND_RIGHT
#    else
#        error "updater: the build must set exactly one of INIT_EE_HANDS_LEFT / INIT_EE_HANDS_RIGHT"
#    endif

#    if defined(POINTING_DEVICE_IS_PMW3389) + defined(POINTING_DEVICE_IS_PMW3360) + defined(PS2_ENABLE) + defined(AZOTEQ_IQS5XX_TPS43) > 1
#        error "updater: more than one pointing device in this build"
#    elif defined(POINTING_DEVICE_IS_PMW3389)
#        define UPDATE_SELF_POINTING UPDATE_POINTING_PMW3389
#    elif defined(POINTING_DEVICE_IS_PMW3360)
#        define UPDATE_SELF_POINTING UPDATE_POINTING_PMW3360
#    elif defined(PS2_ENABLE)
#        define UPDATE_SELF_POINTING UPDATE_POINTING_TRACKPOINT
#    elif defined(AZOTEQ_IQS5XX_TPS43)
#        define UPDATE_SELF_POINTING UPDATE_POINTING_AZOTEQ
#    else
#        define UPDATE_SELF_POINTING UPDATE_POINTING_NONE
#    endif
#endif
