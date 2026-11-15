#ifndef RGB_MATRIX_H
#define RGB_MATRIX_H

#include <stdint.h>

#define RGB_MATRIX_WIDTH 64
#define RGB_MATRIX_HEIGHT 32
#define RGB_MATRIX_RED (1 << 0)
#define RGB_MATRIX_GREEN (1 << 1)
#define RGB_MATRIX_BLUE (1 << 2)

struct RgbMatrixFrame {
    uint8_t pixels[RGB_MATRIX_HEIGHT][RGB_MATRIX_WIDTH]; // [row][column], 0 = off, low 3 bits = RGB
};

void rgb_matrix_init(void);
void rgb_matrix_refresh(const struct RgbMatrixFrame *frame);

#endif
