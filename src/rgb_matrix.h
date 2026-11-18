#ifndef RGB_MATRIX_H
#define RGB_MATRIX_H

#include <stdbool.h>
#include <stdint.h>

#define RGB_MATRIX_WIDTH 64
#define RGB_MATRIX_HEIGHT 32
// on/off switches for each matrix 
#define RGB_MATRIX_RED (1 << 0)
#define RGB_MATRIX_GREEN (1 << 1)
#define RGB_MATRIX_BLUE (1 << 2)

// set up the pins and PIO, return false if setup fails
bool rgb_matrix_init(void);

// read one picture as pixels[row][column] and send it to the panel
// the width tells C how many entries to skip to reach the next row
void rgb_matrix_refresh(uint8_t pixels[][RGB_MATRIX_WIDTH]);

#endif
