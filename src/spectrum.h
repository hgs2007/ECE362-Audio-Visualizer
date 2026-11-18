#ifndef SPECTRUM_H
#define SPECTRUM_H

#include <stdint.h>

#define SPECTRUM_SAMPLE_COUNT 256 // a power of two lets the FFT double its group size each pass
#define SPECTRUM_BAR_COUNT 16 // one value for each bar on the matrix

// fill the tables once before taking audio readings
void spectrum_init(void);
// use the given block average to center the samples and fill the bar strengths
void spectrum_measure(const uint16_t samples[SPECTRUM_SAMPLE_COUNT], int mean,
                      float amplitudes[SPECTRUM_BAR_COUNT]);

#endif
