// Host stub of ChibiOS hal.h: just enough to compile the real
// lib/chibios/os/hal/src/hal_buffers.c on the host (single thread, no waits).
#pragma once

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int32_t  msg_t;
typedef uint32_t sysinterval_t;
typedef struct {
    int unused;
} threads_queue_t;

#define MSG_OK 0
#define MSG_TIMEOUT -1
#define MSG_RESET -2
#define TIME_IMMEDIATE ((sysinterval_t)0)
#define TIME_INFINITE ((sysinterval_t)-1)

#define osalDbgCheck(c) assert(c)
#define osalDbgAssert(c, r) assert(c)
#define osalDbgCheckClassI()
#define osalDbgCheckClassS()
#define osalSysLock()
#define osalSysUnlock()
#define osalThreadQueueObjectInit(q) ((void)(q))
#define osalThreadDequeueAllI(q, m) ((void)(q))
#define osalThreadDequeueNextI(q, m) ((void)(q))
// Nothing else runs: waiting for a packet always times out.
#define osalThreadEnqueueTimeoutS(q, t) ((void)(q), (void)(t), (msg_t)MSG_TIMEOUT)

#include "hal_buffers.h"
