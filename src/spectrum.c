#include "spectrum.h"

#include <math.h>

struct Complex {
    float real;
    float imag;
};

static const float TWO_PI = 6.28318530718f;
static float hann_window[SPECTRUM_SAMPLE_COUNT];
static struct Complex rotations[SPECTRUM_SAMPLE_COUNT / 2];
static float centered_samples[SPECTRUM_SAMPLE_COUNT];
static struct Complex frequency_bins[SPECTRUM_SAMPLE_COUNT];

// Bins are 62.5 Hz apart (16000 / 256). Bin 0 is the DC level, so start at 1.
// Each number is the first bin of a bar. 128 marks the end of the last bar.
static const int bin_edges[SPECTRUM_BAR_COUNT + 1] = {
    1, 2, 3, 4, 5, 6, 8, 10, 13, 16, 21, 27, 35, 45, 58, 77, 128,
};

// Work out the Hann window and sine/cosine weights once at boot for reuse on every block.
void spectrum_init(void)
{
    for (int i = 0; i < SPECTRUM_SAMPLE_COUNT; ++i) {
        float phase = TWO_PI * i / SPECTRUM_SAMPLE_COUNT;
        hann_window[i] = 0.5f - 0.5f * cosf(phase);
    }
    for (int k = 0; k < SPECTRUM_SAMPLE_COUNT / 2; ++k) {
        float phase = -TWO_PI * k / SPECTRUM_SAMPLE_COUNT;
        rotations[k] = (struct Complex){cosf(phase), sinf(phase)};
    }
}

// Split into even and odd samples, then combine the smaller FFTs into frequency bins.
static void fft(const float *samples, int stride, int count, struct Complex *bins)
{
    if (count == 1) {
        bins[0] = (struct Complex){samples[0], 0.0f};
        return;
    }

    // Doubling stride lets us read the even and odd samples without copying them.
    int half = count / 2;
    fft(samples, stride * 2, half, bins);
    fft(samples + stride, stride * 2, half, bins + half);

    int rotation_step = SPECTRUM_SAMPLE_COUNT / count;
    for (int k = 0; k < half; ++k) {
        struct Complex even = bins[k];
        struct Complex odd = bins[k + half];
        struct Complex rotation = rotations[k * rotation_step];
        // Line up the phase of the odd bins before adding and subtracting.
        float real = rotation.real * odd.real - rotation.imag * odd.imag;
        float imag = rotation.real * odd.imag + rotation.imag * odd.real;
        bins[k] = (struct Complex){even.real + real, even.imag + imag};
        bins[k + half] = (struct Complex){even.real - real, even.imag - imag};
    }
}

// Center and window the mic samples, run the FFT, then find the strongest bin for each bar.
void spectrum_measure(const uint16_t samples[SPECTRUM_SAMPLE_COUNT], int mean,
                      float amplitudes[SPECTRUM_BAR_COUNT])
{
    // Subtract the mic's idle level and fade the ends so the block doesn't cut off sharply.
    for (int i = 0; i < SPECTRUM_SAMPLE_COUNT; ++i) {
        centered_samples[i] = (samples[i] - mean) * hann_window[i];
    }
    fft(centered_samples, 1, SPECTRUM_SAMPLE_COUNT, frequency_bins);

    for (int bar = 0; bar < SPECTRUM_BAR_COUNT; ++bar) {
        float peak_power = 0.0f;
        for (int bin = bin_edges[bar]; bin < bin_edges[bar + 1]; ++bin) {
            struct Complex value = frequency_bins[bin];
            float power = value.real * value.real + value.imag * value.imag;
            if (power > peak_power) {
                peak_power = power;
            }
        }
        // Use the strongest bin in this bar. The scale puts it back in ADC counts.
        amplitudes[bar] = sqrtf(peak_power) * (4.0f / SPECTRUM_SAMPLE_COUNT);
    }
}
