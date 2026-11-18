#include "spectrum.h"

#include <math.h>

static const float TWO_PI = 6.28318530718f; // a full turn in radians for sinf and cosf
static float hann_window[SPECTRUM_SAMPLE_COUNT]; // one multiplier per sample to fade the ends
static int sample_order[SPECTRUM_SAMPLE_COUNT]; // where to put each sample before the FFT
static float fft_cos[SPECTRUM_SAMPLE_COUNT / 2]; // the biggest group needs rotation angles 0 through 127
static float fft_sin[SPECTRUM_SAMPLE_COUNT / 2];
// store a pair of numbers per slot so we can track strength and phase, or where the wave is in its cycle
static float fft_real[SPECTRUM_SAMPLE_COUNT];
static float fft_imag[SPECTRUM_SAMPLE_COUNT];

// a bin is one frequency in the FFT, spaced 16000 / 256 = 62.5 Hz apart
// skip bin 0 because it measures the steady voltage instead of a sound frequency
// each bar starts at bin_edges[bar] and stops before the next edge, so the last ends at 127
static const int bin_edges[SPECTRUM_BAR_COUNT + 1] = {
    1, 2, 3, 4, 5, 6, 8, 10, 13, 16, 21, 27, 35, 45, 58, 77, 128,
};

// flip the order of the index bits so the first FFT pairs are next to each other
// for 256 samples, 00000001 becomes 10000000
static int reverse_bits(int index)
{
    int reversed = 0; // build the result starting with all bits off
    // the first destination bit is 128, the leftmost bit of an 8-bit index
    for(int bit = SPECTRUM_SAMPLE_COUNT / 2; bit > 0; bit >>= 1)
    {
        if(index & 1) // check the rightmost bit of the old index
        {
            reversed |= bit; // turn on its matching bit on the other end
        }
        index >>= 1; // move the next source bit into the rightmost spot
    }
    return reversed;
}

// fill these tables at startup so we can reuse them for each block
void spectrum_init(void)
{
    for(int i = 0; i < SPECTRUM_SAMPLE_COUNT; ++i)
    {
        float phase = TWO_PI * i / SPECTRUM_SAMPLE_COUNT; // go around one turn across the block
        // at the start cos is 1, so the multiplier is 0, in the middle cos is -1 so it's 1
        hann_window[i] = 0.5f - 0.5f * cosf(phase);
        sample_order[i] = reverse_bits(i); // with four samples this puts them in order 0, 2, 1, 3
    }
    for(int i = 0; i < SPECTRUM_SAMPLE_COUNT / 2; ++i)
    {
        float angle = -TWO_PI * i / SPECTRUM_SAMPLE_COUNT; // negative for the forward FFT
        fft_cos[i] = cosf(angle); // real part of the rotation
        fft_sin[i] = sinf(angle); // imaginary part of the same rotation
    }
}

// combine pairs first, then groups of 4, 8, and so on
// after the last pass, each array index is a frequency bin
static void run_fft(void)
{
    for(int group_size = 2; group_size <= SPECTRUM_SAMPLE_COUNT; group_size *= 2)
    {
        int half = group_size / 2; // size of each smaller FFT we're combining
        // a group of 8 needs angles 2 pi / 8 apart, so use every 256 / 8 = 32nd table entry
        int weight_step = SPECTRUM_SAMPLE_COUNT / group_size;

        // step past each finished group until we've covered the whole block
        for(int start = 0; start < SPECTRUM_SAMPLE_COUNT; start += group_size)
        {
            for(int i = 0; i < half; ++i)
            {
                // match up the same position in both halves of the group
                int left = start + i; // position in the first half
                int right = left + half; // same position in the second half
                int weight = i * weight_step; // table entry for this pair's angle

                // rotate the right half so we account for the original spacing of the samples
                // (a + jb) times (cos + j sin) gives a*cos - b*sin and a*sin + b*cos
                // the minus sign comes from j times j being -1
                float cosine = fft_cos[weight];
                float sine = fft_sin[weight];
                float right_real = fft_real[right] * cosine - fft_imag[right] * sine;
                float right_imag = fft_real[right] * sine + fft_imag[right] * cosine;

                // one new bin adds the rotated result and the other subtracts it
                // write the difference first so we can still use the old left value for the sum
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
    // the window fades the ends so a cut-off wave spreads less into nearby bins
    for(int i = 0; i < SPECTRUM_SAMPLE_COUNT; ++i)
    {
        int slot = sample_order[i]; // place this sample using the order we made at startup
        fft_real[slot] = (samples[i] - mean) * hann_window[i]; // move the signal around zero before multiplying
        fft_imag[slot] = 0.0f; // the ADC samples have no imaginary part
    }
    run_fft(); // the two arrays now hold the real and imaginary parts of each frequency bin

    for(int bar = 0; bar < SPECTRUM_BAR_COUNT; ++bar)
    {
        float peak_power = 0.0f; // compare bins using squared magnitude and take just one square root later
        for(int bin = bin_edges[bar]; bin < bin_edges[bar + 1]; ++bin)
        {
            float real = fft_real[bin];
            float imag = fft_imag[bin];
            float power = real * real + imag * imag; // like 3*3 + 4*4 = 25, whose square root is 5
            if(power > peak_power)
            {
                peak_power = power;
            }
        }
        // divide by 256 for the FFT sum, then double because real input splits a tone between two matching bins
        // double again because the Hann multipliers average to 0.5, giving 4 / 256 in all
        amplitudes[bar] = sqrtf(peak_power) * (4.0f / SPECTRUM_SAMPLE_COUNT);
    }
}
