// Host build of keyboards/svalboard/debounce_sval.c (tests/sval_debounce/test_debounce.c).
#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef uint8_t matrix_row_t;
#define MATRIX_ROWS_PER_HAND 5
#define DOUBLEDOWN_COL 5
uint16_t host_now_ms(void);
void     debounce_init(void);
bool     debounce(matrix_row_t raw[], matrix_row_t cooked[], bool changed);
bool     sval_debounce_step(matrix_row_t raw[], matrix_row_t cooked[], uint16_t now_ms);
