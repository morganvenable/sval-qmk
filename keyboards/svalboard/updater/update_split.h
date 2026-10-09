// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// In-firmware updater, M2: the split link (plan docs/updater-plan.md, M2).
// Presence with version (D17, V) and the KEYBOARD_UPDATE split RPC; the wire
// format is in update_split_wire.h.
//
// M2a stage 1 (this file): presence, mismatch detection, and a slave-side
// KEYBOARD_UPDATE handler that checks every request, answers INFO, STATUS and
// ABORT, and answers retries from its cache. BEGIN, PAGE, END and COMMIT are
// checked and then refused with UNSUPPORTED: the mailbox, programming and the
// master's relay come in stage 2.

#include <stdbool.h>
#include <stdint.h>
#include "update_split_wire.h"

// ---- presence (D17) ----------------------------------------------------------------

// The half without USB, inside the KEYBOARD_SYNC_A RPC callback (SlaveThread):
// fill the response with this build's presence. Writes nothing if out is
// shorter than UPDATE_PRESENCE_BYTES.
void update_split_presence_fill(void *out, uint8_t out_len);

// The half with USB, after each presence ping: answered is the RPC's result,
// rsp its response bytes.
void update_split_presence_result(bool answered, const void *rsp, uint8_t len);

// update_presence_status_t: NONE until the other half answers, and again once
// a ping goes unanswered.
uint8_t update_split_presence(void);

// The other half answers and is not a match (VERSION, SAME_HAND or INVALID):
// the red error LED while the updater is idle, INFO flags bit5 (V).
bool update_split_mismatch(void);

// ---- KEYBOARD_UPDATE, the half without USB ---------------------------------------------

// The RPC callback body: one request frame in, one response frame out
// (out_len bytes, zero-filled past the frame). Runs in the SlaveThread
// (1 KiB stack) while the master waits; never touches flash.
void update_split_slave_rpc(const uint8_t *in, uint8_t in_len, uint8_t *out, uint8_t out_len);

// update_state_t of the slave's split session (IDLE in stage 1).
uint8_t update_split_slave_state(void);

#ifdef SVAL_UPDATER_HOST_TEST
void update_split_host_reset(void);
#else
// keyboard_post_init (main thread): probes the flash die once, so the
// SlaveThread never does, and registers the KEYBOARD_UPDATE callback.
void update_split_init(void);
#endif

// ---- port (firmware at the end of update_split.c; the host tests supply their own) ----

void    update_split_port_build_id(update_build_id_t *id);
uint8_t update_split_port_info_flags(void); // INFO flags byte, see update_split_wire.h
bool    update_split_port_updating(void);   // updater_active()
