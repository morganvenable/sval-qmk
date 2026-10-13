// Copyright 2026 Svalboard
// SPDX-License-Identifier: GPL-3.0-or-later OR Apache-2.0

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * Takes the next received USB packet out of an input buffers queue as one
 * unit: copies at most `size` bytes of it into `data`, zero-fills the rest of
 * `data`, sets *received to the packet's own length and frees its buffer.
 * Returns false when no packet is queued.
 *
 * ibqReadTimeout() reads a byte stream instead, so a short packet (a host
 * sending 32-byte reports to a 64-byte endpoint) would be joined with the
 * start of the next packet, and every later report would be misframed.
 *
 * Call with the system locked (S-class).
 */
static inline bool ibq_take_packet_s(input_buffers_queue_t *ibqp, uint8_t *data, size_t size, size_t *received) {
    if (ibqGetFullBufferTimeoutS(ibqp, TIME_IMMEDIATE) != MSG_OK) {
        return false;
    }
    size_t n  = (size_t)(ibqp->top - ibqp->ptr);
    *received = n;
    if (n > size) {
        n = size;
    }
    memcpy(data, ibqp->ptr, n);
    memset(data + n, 0, size - n);
    ibqReleaseEmptyBufferS(ibqp);
    return true;
}
