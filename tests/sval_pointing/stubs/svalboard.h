#pragma once
#include <stdbool.h>
#include <stdint.h>
bool sval_other_half_connected(void);
typedef struct { char bytes[72]; } saved_values_t;
typedef struct {
    uint8_t global_bits;
    uint8_t axis_state;
    bool    left_scroll, right_scroll, left_automouse, right_automouse;
} sval_pointing_modes_t;
void sval_pointing_modes(sval_pointing_modes_t *m);
