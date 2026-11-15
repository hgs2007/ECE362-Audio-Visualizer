#include <stdio.h>
#include <string.h>

#include "hardware/adc.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "pico/stdlib.h"
#include "eeprom.h"
#include "rgb_matrix.h"
#include "lcd.h"
#include "spectrum.h"

#define AUDIO_BLOCK_SAMPLES SPECTRUM_SAMPLE_COUNT
#define SAMPLES_PER_COLUMN (AUDIO_BLOCK_SAMPLES / RGB_MATRIX_WIDTH)
#define BAND_COUNT 3

static const int MIC_GPIO = 40; // MAX4466 OUT
static const int MODE_BUTTON_GPIO = 21;
static const int COLOR_BUTTON_GPIO = 26;
static const int STATUS_LED_GPIO = 22;
static const int REPORT_PAUSE_MS = 250;
static const int FIRST_ADC_GPIO = 40;
static const int AUDIO_SAMPLE_RATE_HZ = 16000;
static const int ADC_COUNTS_PER_ROW = 32;
static const int SPECTRUM_NOISE_FLOOR = 6;
static const int SPECTRUM_COUNTS_PER_ROW = 8;
static const int ADC_MAX_COUNT = 4095;
static const int BASS_BAR_COUNT = 4;
static const int TREBLE_FIRST_BAR = 12; // Bars are numbered 0 to 15
static const int WAVE_MID_ROWS = 3;
static const int WAVE_PEAK_ROWS = 8;

struct AudioStats {
    int min;
    int max;
    int mean;
};

static uint16_t audio_samples[AUDIO_BLOCK_SAMPLES];
static int audio_dma_channel;

static const int BASS = 0;
static const int MIDRANGE = 1;
static const int TREBLE = 2;

static const uint8_t band_colors[BAND_COUNT] = {
    RGB_MATRIX_RED, RGB_MATRIX_GREEN, RGB_MATRIX_BLUE,
};

// Start the ADC on GP40 and let DMA fill an array with mic samples
static void mic_init(void)
{
    adc_init();
    adc_gpio_init(MIC_GPIO);
    // The Proton's ADC channels start at GP40, so GP40 is channel 0
    adc_select_input(MIC_GPIO - FIRST_ADC_GPIO);
    adc_fifo_setup(true,   // Store samples in the FIFO
                   true,   // Let the FIFO request a DMA transfer
                   1,      // Request as soon as one sample is ready
                   false,  // Leave the error flag out of the sample
                   false); // Keep all 12 bits of the reading

    // Free-running samples are spaced by 1 + divider ADC clock cycles
    adc_set_clkdiv((float)clock_get_hz(clk_adc) / AUDIO_SAMPLE_RATE_HZ - 1.0f);

    // Each request from the ADC moves one reading into the array
    audio_dma_channel = dma_claim_unused_channel(true);
    dma_channel_config config = dma_channel_get_default_config(audio_dma_channel);
    channel_config_set_transfer_data_size(&config, DMA_SIZE_16); // 12 bits fit in a halfword
    channel_config_set_read_increment(&config, false); // Read the same FIFO register
    channel_config_set_write_increment(&config, true); // Move to the next array slot
    channel_config_set_dreq(&config, DREQ_ADC);
    dma_channel_configure(audio_dma_channel, &config, audio_samples, &adc_hw->fifo,
                          AUDIO_BLOCK_SAMPLES, true);
    adc_run(true);
}

// Use the same view names on the LCD and in Serial Monitor
static const char *mode_name(bool show_spectrum)
{
    return show_spectrum ? "Spectrum" : "Waveform";
}

// Give the full name of the color choice in Serial Monitor
static const char *color_name(bool band_mix_on)
{
    return band_mix_on ? "Band Mix" : "RGB";
}

// Show what each button controls and which option is active
static void show_controls(bool show_spectrum, bool band_mix_on)
{
    char top[LCD_COLUMNS + 1];
    snprintf(top, sizeof(top), "21 View:%s", mode_name(show_spectrum));
    const char *bottom = band_mix_on ? "26 Color:Mix" : "26 Color:RGB";
    lcd_show_lines(top, bottom);
}

// The left four bars are bass, the middle eight are mids, and the right four are treble
static int band_for_bar(int bar)
{
    if (bar < BASS_BAR_COUNT) {
        return BASS;
    }
    if (bar >= TREBLE_FIRST_BAR) {
        return TREBLE;
    }
    return MIDRANGE;
}

// Find the loudest bar in each range and mix the colors of the strongest ranges
static uint8_t mix_band_colors(const float amplitudes[SPECTRUM_BAR_COUNT])
{
    float peaks[BAND_COUNT] = {0};
    float strongest = 0.0f;
    for (int bar = 0; bar < SPECTRUM_BAR_COUNT; ++bar) {
        int band = band_for_bar(bar);
        if (amplitudes[bar] > peaks[band]) {
            peaks[band] = amplitudes[bar];
        }
        if (amplitudes[bar] > strongest) {
            strongest = amplitudes[bar];
        }
    }

    // Include any band that's above the noise floor and at least half the strongest band
    uint8_t mixed = 0;
    for (int band = 0; band < BAND_COUNT; ++band) {
        if (peaks[band] > SPECTRUM_NOISE_FLOOR && peaks[band] >= strongest * 0.5f) {
            mixed |= band_colors[band]; // OR turns on more than one LED channel at a time
        }
    }
    return mixed ? mixed : RGB_MATRIX_BLUE; // Keep a visible trace in a quiet room
}

// Plot one out of every four samples so a block of 256 fits the 64 columns
static void draw_waveform(struct RgbMatrixFrame *frame, int mean, uint8_t mixed)
{
    for (int x = 0; x < RGB_MATRIX_WIDTH; ++x) {
        int centered = audio_samples[x * SAMPLES_PER_COLUMN] - mean;
        int offset = centered / ADC_COUNTS_PER_ROW;
        int row = RGB_MATRIX_HEIGHT / 2 - offset; // Row 0 is at the top
        // Clip peaks that would go past the top or bottom
        if (row < 0) {
            row = 0;
        } else if (row >= RGB_MATRIX_HEIGHT) {
            row = RGB_MATRIX_HEIGHT - 1;
        }
        uint8_t color = mixed;
        if (!color) {
            // Use distance above or below the center for the three amplitude colors
            int distance = offset < 0 ? -offset : offset;
            color = RGB_MATRIX_BLUE;
            if (distance >= WAVE_PEAK_ROWS) {
                color = RGB_MATRIX_RED;
            } else if (distance >= WAVE_MID_ROWS) {
                color = RGB_MATRIX_GREEN;
            }
        }
        frame->pixels[row][x] = color;
    }
}

// Turn the 16 frequency readings into bars that grow up from the bottom
static void draw_spectrum(struct RgbMatrixFrame *frame, const float amplitudes[SPECTRUM_BAR_COUNT],
                          uint8_t mixed)
{
    // Each bar has three colored columns and a one-column gap
    const int columns_per_bar = RGB_MATRIX_WIDTH / SPECTRUM_BAR_COUNT;
    for (int bar = 0; bar < SPECTRUM_BAR_COUNT; ++bar) {
        float amplitude = amplitudes[bar] - SPECTRUM_NOISE_FLOOR;
        if (amplitude <= 0.0f) {
            continue;
        }
        int height = (int)(amplitude / SPECTRUM_COUNTS_PER_ROW);
        if (height > RGB_MATRIX_HEIGHT) {
            height = RGB_MATRIX_HEIGHT;
        }
        uint8_t color = mixed ? mixed : band_colors[band_for_bar(bar)];
        int first_column = bar * columns_per_bar;
        for (int row = RGB_MATRIX_HEIGHT - height; row < RGB_MATRIX_HEIGHT; ++row) {
            for (int x = first_column; x < first_column + columns_per_bar - 1; ++x) {
                frame->pixels[row][x] = color;
            }
        }
    }
}

// Use the completed DMA block to draw a new picture, then start filling the sample array again
static struct AudioStats finish_audio_block(struct RgbMatrixFrame *frame, bool show_spectrum,
                                            bool band_mix_on)
{
    // DMA is done here, and this SDK call keeps the compiler from reading the array too early
    dma_channel_wait_for_finish_blocking(audio_dma_channel);
    adc_run(false);
    adc_fifo_drain(); // Drop any samples that arrived after the array filled

    int min = ADC_MAX_COUNT;
    int max = 0;
    int sum = 0;

    for (int i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
        int sample = audio_samples[i];
        if (sample < min) {
            min = sample;
        }
        if (sample > max) {
            max = sample;
        }
        sum += sample;
    }
    int mean = sum / AUDIO_BLOCK_SAMPLES; // The mic sits around half of 3.3 V at idle

    memset(frame, 0, sizeof(*frame));
    float amplitudes[SPECTRUM_BAR_COUNT];
    // We need the frequency readings for bar heights or for mixing colors
    if (show_spectrum || band_mix_on) {
        spectrum_measure(audio_samples, mean, amplitudes);
    }
    uint8_t mixed = band_mix_on ? mix_band_colors(amplitudes) : 0;
    if (show_spectrum) {
        draw_spectrum(frame, amplitudes, mixed);
    } else {
        draw_waveform(frame, mean, mixed);
    }

    // Point DMA back at the start of the array once we've used the old samples
    dma_channel_transfer_to_buffer_now(audio_dma_channel, audio_samples, AUDIO_BLOCK_SAMPLES);
    adc_run(true);
    return (struct AudioStats){.min = min, .max = max, .mean = mean};
}

// Print the range of the latest samples so we can check that the mic reacts to sound
static void report_mic_level(struct AudioStats stats, bool show_spectrum)
{
    printf("mic GP%d: min=%d max=%d mean=%d peak_to_peak=%d mode=%s\n",
           MIC_GPIO, stats.min, stats.max, stats.mean, stats.max - stats.min, mode_name(show_spectrum));
}

// Set up the hardware, then keep checking the buttons, audio, and display in the main loop
int main(void)
{
    stdio_init_all(); // Send printf and receive the 'm' key through the debugger UART
    gpio_init(STATUS_LED_GPIO);
    gpio_set_dir(STATUS_LED_GPIO, GPIO_OUT);
    gpio_init(MODE_BUTTON_GPIO);
    gpio_set_dir(MODE_BUTTON_GPIO, GPIO_IN);
    gpio_init(COLOR_BUTTON_GPIO);
    gpio_set_dir(COLOR_BUTTON_GPIO, GPIO_IN);

    rgb_matrix_init();
    eeprom_init();
    bool show_spectrum;
    bool band_mix_on;
    bool restored = eeprom_load_choices(&show_spectrum, &band_mix_on);
    printf("Mode: %s (%s). 21 switches view, 26 switches color.\n",
           mode_name(show_spectrum), restored ? "loaded from EEPROM" : "default");
    printf("Color: %s.\n", color_name(band_mix_on));
    lcd_init();
    show_controls(show_spectrum, band_mix_on);

    spectrum_init();
    mic_init();

    bool led_on = false;
    bool mode_was_down = gpio_get(MODE_BUTTON_GPIO);
    bool color_was_down = gpio_get(COLOR_BUTTON_GPIO);
    bool frame_ready = false;
    struct AudioStats stats = {0};
    static struct RgbMatrixFrame frame; // Keep the pixel array off the stack
    absolute_time_t next_report = get_absolute_time();
    while (true) {
        bool mode_down = gpio_get(MODE_BUTTON_GPIO);
        bool color_down = gpio_get(COLOR_BUTTON_GPIO);
        // Act once per new press, even if the button stays down
        bool mode_pressed = mode_down && !mode_was_down;
        bool color_pressed = color_down && !color_was_down;
        mode_was_down = mode_down;
        color_was_down = color_down;
        bool serial_request = getchar_timeout_us(0) == 'm'; // Keep scanning if nobody types

        bool changed = false;
        if (mode_pressed || serial_request) {
            // Apply the new choice only after EEPROM readback succeeds
            if (eeprom_save_view(!show_spectrum)) {
                show_spectrum = !show_spectrum;
                changed = true;
                printf("Saved %s to EEPROM.\n", mode_name(show_spectrum));
            } else {
                puts("Could not save the view to EEPROM at 0x50.");
            }
        }
        if (color_pressed) {
            if (eeprom_save_color(!band_mix_on)) {
                band_mix_on = !band_mix_on;
                changed = true;
                printf("Saved %s to EEPROM.\n", color_name(band_mix_on));
            } else {
                puts("Could not save the color to EEPROM at 0x50.");
            }
        }
        if (changed) {
            frame_ready = false; // Wait for a picture with the new settings
            show_controls(show_spectrum, band_mix_on);
        }

        if (!dma_channel_is_busy(audio_dma_channel)) {
            stats = finish_audio_block(&frame, show_spectrum, band_mix_on);
            frame_ready = true;
        }

        // Limit UART prints so the matrix can keep refreshing between reports
        if (frame_ready && time_reached(next_report)) {
            led_on = !led_on;
            gpio_put(STATUS_LED_GPIO, led_on);
            report_mic_level(stats, show_spectrum);
            next_report = make_timeout_time_ms(REPORT_PAUSE_MS);
        }

        if (frame_ready) {
            rgb_matrix_refresh(&frame);
        } else {
            sleep_ms(1);
        }
    }
}
