#include "spectrum.h"

#include <math.h>

typedef struct {
    float real;
    float imag;
} Complex;

static const float TWO_PI = 6.28318530718f;
static float hann_window[SPECTRUM_SAMPLE_COUNT];
static Complex rotations[SPECTRUM_SAMPLE_COUNT / 2];
static float centered_samples[SPECTRUM_SAMPLE_COUNT];
static Complex frequency_bins[SPECTRUM_SAMPLE_COUNT];

// Bins are 62.5 Hz apart (16000 / 256). Bin 0 is the DC level, so start at 1.
// Each number is the first bin of a bar. 128 marks the end of the last bar.
static const uint8_t bin_edges[SPECTRUM_BAR_COUNT + 1] = {
    1, 2, 3, 4, 5, 6, 8, 10, 13, 16, 21, 27, 35, 45, 58, 77, 128,
};

void spectrum_init(void)
{
    // Calculate the Hann window and FFT weights once at boot.
    for (unsigned i = 0; i < SPECTRUM_SAMPLE_COUNT; ++i) {
        float phase = TWO_PI * i / SPECTRUM_SAMPLE_COUNT;
        hann_window[i] = 0.5f - 0.5f * cosf(phase);
    }
    for (unsigned k = 0; k < SPECTRUM_SAMPLE_COUNT / 2; ++k) {
        float phase = -TWO_PI * k / SPECTRUM_SAMPLE_COUNT;
        rotations[k] = (Complex){cosf(phase), sinf(phase)};
    }
}

static void fft(const float *samples, unsigned stride, unsigned count, Complex *bins)
{
    if (count == 1) {
        bins[0] = (Complex){samples[0], 0.0f};
        return;
    }

    // Doubling stride lets us read the even and odd samples without copying them.
    unsigned half = count / 2;
    fft(samples, stride * 2, half, bins);
    fft(samples + stride, stride * 2, half, bins + half);

    unsigned rotation_step = SPECTRUM_SAMPLE_COUNT / count;
    for (unsigned k = 0; k < half; ++k) {
        Complex even = bins[k];
        Complex odd = bins[k + half];
        Complex rotation = rotations[k * rotation_step];
        // Line up the phase of the odd bins before adding and subtracting.
        float real = rotation.real * odd.real - rotation.imag * odd.imag;
        float imag = rotation.real * odd.imag + rotation.imag * odd.real;
        bins[k] = (Complex){even.real + real, even.imag + imag};
        bins[k + half] = (Complex){even.real - real, even.imag - imag};
    }
}

void spectrum_measure(const uint16_t samples[SPECTRUM_SAMPLE_COUNT], unsigned mean,
                      float amplitudes[SPECTRUM_BAR_COUNT])
{
    // Subtract the mic's idle level and fade the ends so the block doesn't cut off sharply.
    for (unsigned i = 0; i < SPECTRUM_SAMPLE_COUNT; ++i) {
        centered_samples[i] = ((int)samples[i] - (int)mean) * hann_window[i];
    }
    fft(centered_samples, 1, SPECTRUM_SAMPLE_COUNT, frequency_bins);

    for (unsigned bar = 0; bar < SPECTRUM_BAR_COUNT; ++bar) {
        float peak_power = 0.0f;
        for (unsigned bin = bin_edges[bar]; bin < bin_edges[bar + 1]; ++bin) {
            Complex value = frequency_bins[bin];
            float power = value.real * value.real + value.imag * value.imag;
            if (power > peak_power) {
                peak_power = power;
            }
        }
        // Use the strongest bin in this bar. The scale puts it back in ADC counts.
        amplitudes[bar] = sqrtf(peak_power) * (4.0f / SPECTRUM_SAMPLE_COUNT);
    }
}
