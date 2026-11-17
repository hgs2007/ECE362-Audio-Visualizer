#include "spectrum.h"

#include <math.h>

static const float TWO_PI = 6.28318530718f;
static float hann_window[SPECTRUM_SAMPLE_COUNT];
static int sample_order[SPECTRUM_SAMPLE_COUNT];
static float fft_cos[SPECTRUM_SAMPLE_COUNT / 2];
static float fft_sin[SPECTRUM_SAMPLE_COUNT / 2];
// two arrays for the cosine and sine parts of each bin
static float fft_real[SPECTRUM_SAMPLE_COUNT];
static float fft_imag[SPECTRUM_SAMPLE_COUNT];

// bins are 62.5 Hz apart, skip the DC level at bin 0
// each number starts a new bar, 128 is where the last one ends
static const int bin_edges[SPECTRUM_BAR_COUNT + 1] = {
    1, 2, 3, 4, 5, 6, 8, 10, 13, 16, 21, 27, 35, 45, 58, 77, 128,
};

// flip the order of the index bits so the first FFT pairs are next to each other
// for 256 samples, 00000001 becomes 10000000
static int reverse_bits(int index)
{
    int reversed = 0;
    for (int bit = SPECTRUM_SAMPLE_COUNT / 2; bit > 0; bit >>= 1) {
        if (index & 1) {
            reversed |= bit;
        }
        index >>= 1;
    }
    return reversed;
}

// fill these tables at startup so we can reuse them for each block
void spectrum_init(void)
{
    for (int i = 0; i < SPECTRUM_SAMPLE_COUNT; ++i) {
        float phase = TWO_PI * i / SPECTRUM_SAMPLE_COUNT;
        hann_window[i] = 0.5f - 0.5f * cosf(phase);
        sample_order[i] = reverse_bits(i);
    }
    for (int i = 0; i < SPECTRUM_SAMPLE_COUNT / 2; ++i) {
        float angle = -TWO_PI * i / SPECTRUM_SAMPLE_COUNT; // negative for the forward FFT
        fft_cos[i] = cosf(angle);
        fft_sin[i] = sinf(angle);
    }
}

// combine pairs first, then groups of 4, 8, and so on
// after the last pass, each array index is a frequency bin
static void run_fft(void)
{
    for (int group_size = 2; group_size <= SPECTRUM_SAMPLE_COUNT; group_size *= 2) {
        int half = group_size / 2;
        // smaller groups take bigger steps through the same sine/cosine table
        int weight_step = SPECTRUM_SAMPLE_COUNT / group_size;

        for (int start = 0; start < SPECTRUM_SAMPLE_COUNT; start += group_size) {
            for (int i = 0; i < half; ++i) {
                // match up the same position in both halves of the group
                int left = start + i;
                int right = left + half;
                int weight = i * weight_step;

                // adjust the phase of the right half before combining it with the left
                float cosine = fft_cos[weight];
                float sine = fft_sin[weight];
                float right_real = fft_real[right] * cosine - fft_imag[right] * sine;
                float right_imag = fft_real[right] * sine + fft_imag[right] * cosine;

                // put the sum on the left and the difference on the right
                fft_real[right] = fft_real[left] - right_real;
                fft_imag[right] = fft_imag[left] - right_imag;
                fft_real[left] += right_real;
                fft_imag[left] += right_imag;
            }
        }
    }
}

// turn a block of mic samples into one strength value for each bar
void spectrum_measure(const uint16_t samples[SPECTRUM_SAMPLE_COUNT], int mean,
                      float amplitudes[SPECTRUM_BAR_COUNT])
{
    // subtract the average, fade the ends, and put the samples in FFT order
    for (int i = 0; i < SPECTRUM_SAMPLE_COUNT; ++i) {
        int slot = sample_order[i];
        fft_real[slot] = (samples[i] - mean) * hann_window[i];
        fft_imag[slot] = 0.0f; // the ADC samples have no imaginary part
    }
    run_fft();

    for (int bar = 0; bar < SPECTRUM_BAR_COUNT; ++bar) {
        float peak_power = 0.0f;
        for (int bin = bin_edges[bar]; bin < bin_edges[bar + 1]; ++bin) {
            float real = fft_real[bin];
            float imag = fft_imag[bin];
            float power = real * real + imag * imag;
            if (power > peak_power) {
                peak_power = power;
            }
        }
        // get the magnitude of the strongest bin and scale it back to ADC counts
        amplitudes[bar] = sqrtf(peak_power) * (4.0f / SPECTRUM_SAMPLE_COUNT);
    }
}
