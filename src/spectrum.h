#ifndef SPECTRUM_H
#define SPECTRUM_H

#include <stdint.h>

enum {
    SPECTRUM_SAMPLE_COUNT = 256,
    SPECTRUM_BAR_COUNT = 16,
};

void spectrum_init(void);
void spectrum_measure(const uint16_t samples[SPECTRUM_SAMPLE_COUNT], unsigned mean,
                      float amplitudes[SPECTRUM_BAR_COUNT]);

#endif
