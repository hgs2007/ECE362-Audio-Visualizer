#ifndef SPECTRUM_H
#define SPECTRUM_H

#include <stdint.h>

#define SPECTRUM_SAMPLE_COUNT 256 // the FFT needs a power of two
#define SPECTRUM_BAR_COUNT 16

void spectrum_init(void);
void spectrum_measure(const uint16_t samples[SPECTRUM_SAMPLE_COUNT], int mean,
                      float amplitudes[SPECTRUM_BAR_COUNT]);

#endif
