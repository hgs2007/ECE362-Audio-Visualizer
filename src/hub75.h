#ifndef HUB75_H
#define HUB75_H

#include <stdint.h>

enum {
    HUB75_WIDTH = 64,
    HUB75_HEIGHT = 32,
    HUB75_RED = 1 << 0,
    HUB75_GREEN = 1 << 1,
    HUB75_BLUE = 1 << 2,
};

typedef struct {
    uint8_t pixels[HUB75_HEIGHT][HUB75_WIDTH]; // [row][column], 0 = off, low 3 bits = RGB
} Hub75Frame;

void hub75_init(void);
void hub75_refresh(const Hub75Frame *frame);

#endif
