#ifndef HUB75_H
#define HUB75_H

#include <stdint.h>

enum {
    HUB75_WIDTH = 64,
    HUB75_HEIGHT = 32,
};

void hub75_init(void);
void hub75_refresh_waveform(const uint8_t rows[HUB75_WIDTH]);

#endif
