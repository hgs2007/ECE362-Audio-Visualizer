#include "spectrum.h"

#include <math.h>

static const float TWO_PI = 6.28318530718f;
static float hann_window[SPECTRUM_SAMPLE_COUNT];
static int sample_order[SPECTRUM_SAMPLE_COUNT];
static float fft_cos[SPECTRUM_SAMPLE_COUNT / 2];
static float fft_sin[SPECTRUM_SAMPLE_COUNT / 2];
// Each bin uses one number for the cosine part and one for the sine part
static float fft_real[SPECTRUM_SAMPLE_COUNT];
static float fft_imag[SPECTRUM_SAMPLE_COUNT];

// Bins are 62.5 Hz apart, with bin 0 skipped because it is the DC level
// Each number starts a bar, and 128 is the stop point for the last bar
static const int bin_edges[SPECTRUM_BAR_COUNT + 1] = {
    1, 2, 3, 4, 5, 6, 8, 10, 13, 16, 21, 27, 35, 45, 58, 77, 128,
};

// Read the index bits from right to left and write them from left to right
// For a block of 256, index 00000001 goes to slot 10000000
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

// Work out the window, sample order, and sine/cosine weights once at startup
void spectrum_init(void)
{
    for (int i = 0; i < SPECTRUM_SAMPLE_COUNT; ++i) {
        float phase = TWO_PI * i / SPECTRUM_SAMPLE_COUNT;
        hann_window[i] = 0.5f - 0.5f * cosf(phase);
        sample_order[i] = reverse_bits(i);
    }
    for (int i = 0; i < SPECTRUM_SAMPLE_COUNT / 2; ++i) {
        float angle = -TWO_PI * i / SPECTRUM_SAMPLE_COUNT; // Negative for the forward FFT
        fft_cos[i] = cosf(angle);
        fft_sin[i] = sinf(angle);
    }
}

// Start by combining pairs, then join the results into groups of 4, 8, and so on
// After the last pass, each array index is a frequency bin
static void run_fft(void)
{
    for (int group_size = 2; group_size <= SPECTRUM_SAMPLE_COUNT; group_size *= 2) {
        int half = group_size / 2;
        // Use the same angle table for every pass, e.g. a group of 4 uses every 64th entry
        int weight_step = SPECTRUM_SAMPLE_COUNT / group_size;

        for (int start = 0; start < SPECTRUM_SAMPLE_COUNT; start += group_size) {
            for (int i = 0; i < half; ++i) {
                // Pair the same position in the first and second halves of this group
                int left = start + i;
                int right = left + half;
                int weight = i * weight_step;

                // Rotate the right-hand result so its sine and cosine line up with the left
                float cosine = fft_cos[weight];
                float sine = fft_sin[weight];
                float right_real = fft_real[right] * cosine - fft_imag[right] * sine;
                float right_imag = fft_real[right] * sine + fft_imag[right] * cosine;

                // The sum and difference are the two results for this pair
                fft_real[right] = fft_real[left] - right_real;
                fft_imag[right] = fft_imag[left] - right_imag;
                fft_real[left] += right_real;
                fft_imag[left] += right_imag;
            }
        }
    }
}

// Take one block of mic samples and turn it into 16 bar amplitudes
void spectrum_measure(const uint16_t samples[SPECTRUM_SAMPLE_COUNT], int mean,
                      float amplitudes[SPECTRUM_BAR_COUNT])
{
    // Center the samples, fade the edges, and put them where the first FFT pass expects them
    for (int i = 0; i < SPECTRUM_SAMPLE_COUNT; ++i) {
        int slot = sample_order[i];
        fft_real[slot] = (samples[i] - mean) * hann_window[i];
        fft_imag[slot] = 0.0f; // Mic readings start out as real numbers
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
        // Sqrt gives the magnitude of the winning bin, and 4/N puts it back in ADC counts
        amplitudes[bar] = sqrtf(peak_power) * (4.0f / SPECTRUM_SAMPLE_COUNT);
    }
}
