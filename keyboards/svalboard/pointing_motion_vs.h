// The Velocity Scroll block of POINTING_MOTION (Sval 0x2D): its own header,
// free of QMK headers and of the generated fragment ids, so svalboard.h can
// carry the type and tests/sval_pointing can build it on the host.
#pragma once

#include <stdint.h>

// ------------------------------------------------- Velocity Scroll block
// Bytes 29-52 of the 0x2D reply (docs/protocol.md, "Velocity Scroll"), after
// the per-reader extension's place (24-28) so the two never overlap. Only in a
// 64-byte report. Firmware without it leaves the bytes zero, so the magic byte
// tells a host the block is there.
#define SVAL_MOTION_VS_OFF   29
#define SVAL_MOTION_VS_LEN   24
#define SVAL_MOTION_VS_MAGIC 0x56 // 'V': the block is present, format 1

// Block byte 1.
#define SVAL_MOTION_VS_HELD       0x01 // Velocity Scroll is held
#define SVAL_MOTION_VS_TOUCHING   0x02 // a finger on a pad sets the offset (else the relative pointers)
#define SVAL_MOTION_VS_RIGHT_PAD  0x04 // that pad is the right hand's
#define SVAL_MOTION_VS_AXIS_LOCK  0x08 // the axis lock tunable is on
#define SVAL_MOTION_VS_LOCK_SHIFT 4    // bits 4-5: 0 no axis yet, 1 vertical, 2 horizontal

// The state and the tunables, in physical units; packing scales them.
typedef struct {
    uint8_t flags;           // SVAL_MOTION_VS_*
    float   off_x, off_y;    // mm, +right / +toward the user
    float   speed_h, speed_v; // notches/s, +right / +up (before natural scroll)
    float   max_nps;
    float   ball_dead, ball_gain, ball_exp;
    float   pad_dead, pad_gain, pad_exp;
} sval_motion_vs_t;

static inline int32_t sval_motion_vs_round(float v) {
    return (int32_t)(v < 0.0f ? v - 0.5f : v + 0.5f);
}
static inline void sval_motion_vs_put16(uint8_t *out, int32_t v, int32_t lo, int32_t hi) {
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    out[0] = (uint8_t)((uint32_t)v & 0xFF);
    out[1] = (uint8_t)(((uint32_t)v >> 8) & 0xFF);
}

// Writes the 24-byte block. Offsets in 0.01 mm and speeds in 0.01 notch/s
// (int16), the tunables in 0.01 mm, 0.01 notch/s and 0.001 (uint16).
static inline void sval_motion_pack_vs(uint8_t *out, const sval_motion_vs_t *vs) {
    out[0] = SVAL_MOTION_VS_MAGIC;
    out[1] = vs->flags;
    sval_motion_vs_put16(&out[2], sval_motion_vs_round(vs->off_x * 100.0f), INT16_MIN, INT16_MAX);
    sval_motion_vs_put16(&out[4], sval_motion_vs_round(vs->off_y * 100.0f), INT16_MIN, INT16_MAX);
    sval_motion_vs_put16(&out[6], sval_motion_vs_round(vs->speed_h * 100.0f), INT16_MIN, INT16_MAX);
    sval_motion_vs_put16(&out[8], sval_motion_vs_round(vs->speed_v * 100.0f), INT16_MIN, INT16_MAX);
    sval_motion_vs_put16(&out[10], sval_motion_vs_round(vs->max_nps * 100.0f), 0, UINT16_MAX);
    sval_motion_vs_put16(&out[12], sval_motion_vs_round(vs->ball_dead * 100.0f), 0, UINT16_MAX);
    sval_motion_vs_put16(&out[14], sval_motion_vs_round(vs->ball_gain * 100.0f), 0, UINT16_MAX);
    sval_motion_vs_put16(&out[16], sval_motion_vs_round(vs->ball_exp * 1000.0f), 0, UINT16_MAX);
    sval_motion_vs_put16(&out[18], sval_motion_vs_round(vs->pad_dead * 100.0f), 0, UINT16_MAX);
    sval_motion_vs_put16(&out[20], sval_motion_vs_round(vs->pad_gain * 100.0f), 0, UINT16_MAX);
    sval_motion_vs_put16(&out[22], sval_motion_vs_round(vs->pad_exp * 1000.0f), 0, UINT16_MAX);
}
