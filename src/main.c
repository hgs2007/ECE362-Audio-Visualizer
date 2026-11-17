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

static const int MIC_GPIO = 40; // mic output from the MAX4466
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
static const int TREBLE_FIRST_BAR = 12; // bars are numbered 0 to 15
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

// set up the mic ADC and let DMA fill the sample array
static void mic_init(void)
{
    adc_init();
    adc_gpio_init(MIC_GPIO);
    // since ADC channels start at gp40, it's channel 0
    adc_select_input(MIC_GPIO - FIRST_ADC_GPIO);
    adc_fifo_setup(true, // put samples in the FIFO
                   true, // let the FIFO ask DMA for a transfer
                   1, // ask as soon as there's one sample
                   false, // leave out the error bit
                   false); // keep all 12 bits

    // the ADC waits divider + 1 clock cycles between samples
    adc_set_clkdiv((float)clock_get_hz(clk_adc) / AUDIO_SAMPLE_RATE_HZ - 1.0f);

    // one DMA transfer per sample
    audio_dma_channel = dma_claim_unused_channel(true);
    dma_channel_config config = dma_channel_get_default_config(audio_dma_channel);
    channel_config_set_transfer_data_size(&config, DMA_SIZE_16); // 12 bits fit in a halfword
    channel_config_set_read_increment(&config, false); // always read the same FIFO register
    channel_config_set_write_increment(&config, true); // write to the next spot in the array
    channel_config_set_dreq(&config, DREQ_ADC);
    dma_channel_configure(audio_dma_channel, &config, audio_samples, &adc_hw->fifo,
                          AUDIO_BLOCK_SAMPLES, true);
    adc_run(true);
}

// view name for the LCD and serial output
static const char *mode_name(bool show_spectrum)
{
    return show_spectrum ? "Spectrum" : "Waveform";
}

// use the full color name in the serial output
static const char *color_name(bool band_mix_on)
{
    return band_mix_on ? "Band Mix" : "RGB";
}

// put the button numbers and current choices on the LCD
static void show_controls(bool show_spectrum, bool band_mix_on)
{
    char top[LCD_COLUMNS + 1];
    snprintf(top, sizeof(top), "21 View:%s", mode_name(show_spectrum));
    const char *bottom = band_mix_on ? "26 Color:Mix" : "26 Color:RGB";
    lcd_show_lines(top, bottom);
}

// first 4 bars are bass, next 8 are mids, last 4 are treble
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

// use the tallest bar in each band to decide which colors to mix
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

    // add the color if the band clears the noise floor and is at least half the loudest one
    uint8_t mixed = 0;
    for (int band = 0; band < BAND_COUNT; ++band) {
        if (peaks[band] > SPECTRUM_NOISE_FLOOR && peaks[band] >= strongest * 0.5f) {
            mixed |= band_colors[band]; // add this color bit to the ones already on
        }
    }
    return mixed ? mixed : RGB_MATRIX_BLUE; // use blue when it's too quiet to mix
}

// plot every fourth sample so 256 samples fit in 64 columns
static void draw_waveform(struct RgbMatrixFrame *frame, int mean, uint8_t mixed)
{
    for (int x = 0; x < RGB_MATRIX_WIDTH; ++x) {
        int centered = audio_samples[x * SAMPLES_PER_COLUMN] - mean;
        int offset = centered / ADC_COUNTS_PER_ROW;
        int row = RGB_MATRIX_HEIGHT / 2 - offset; // row 0 is at the top
        // keep the peaks inside the screen
        if (row < 0) {
            row = 0;
        } else if (row >= RGB_MATRIX_HEIGHT) {
            row = RGB_MATRIX_HEIGHT - 1;
        }
        uint8_t color = mixed;
        if (!color) {
            // color depends on distance from center in RGB mode
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

// draw the 16 frequency bars from the bottom up
static void draw_spectrum(struct RgbMatrixFrame *frame, const float amplitudes[SPECTRUM_BAR_COUNT],
                          uint8_t mixed)
{
    // three columns for the bar, one for the gap
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

// draw the next frame from the samples we just got, then start another capture
static struct AudioStats finish_audio_block(struct RgbMatrixFrame *frame, bool show_spectrum,
                                            bool band_mix_on)
{
    // this call makes sure the array reads happen after DMA finishes
    dma_channel_wait_for_finish_blocking(audio_dma_channel);
    adc_run(false);
    adc_fifo_drain(); // toss any extra samples left in the FIFO

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
    int mean = sum / AUDIO_BLOCK_SAMPLES; // use the block average as the center line

    memset(frame, 0, sizeof(*frame));
    float amplitudes[SPECTRUM_BAR_COUNT];
    // only run the FFT for the bars or Band Mix
    if (show_spectrum || band_mix_on) {
        spectrum_measure(audio_samples, mean, amplitudes);
    }
    uint8_t mixed = band_mix_on ? mix_band_colors(amplitudes) : 0;
    if (show_spectrum) {
        draw_spectrum(frame, amplitudes, mixed);
    } else {
        draw_waveform(frame, mean, mixed);
    }

    // start filling the same array from the top again
    dma_channel_transfer_to_buffer_now(audio_dma_channel, audio_samples, AUDIO_BLOCK_SAMPLES);
    adc_run(true);
    return (struct AudioStats){.min = min, .max = max, .mean = mean};
}

// print the sample range so we can see if the mic is picking up sound
static void report_mic_level(struct AudioStats stats, bool show_spectrum)
{
    printf("mic GP%d: min=%d max=%d mean=%d peak_to_peak=%d mode=%s\n",
           MIC_GPIO, stats.min, stats.max, stats.mean, stats.max - stats.min, mode_name(show_spectrum));
}

// start the hardware and keep the audio, buttons, and screen running
int main(void)
{
    stdio_init_all(); // use the debugger UART for logs and the 'm' key
    gpio_init(STATUS_LED_GPIO);
    gpio_set_dir(STATUS_LED_GPIO, GPIO_OUT);
    gpio_init(MODE_BUTTON_GPIO);
    gpio_set_dir(MODE_BUTTON_GPIO, GPIO_IN);
    gpio_init(COLOR_BUTTON_GPIO);
    gpio_set_dir(COLOR_BUTTON_GPIO, GPIO_IN);

    if (!rgb_matrix_init()) {
        puts("Could not start the RGB matrix.");
        return 1;
    }
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
    static struct RgbMatrixFrame frame; // keep the pixel array off the stack
    absolute_time_t next_report = get_absolute_time();
    while (true) {
        bool mode_down = gpio_get(MODE_BUTTON_GPIO);
        bool color_down = gpio_get(COLOR_BUTTON_GPIO);
        // held buttons should only count once
        bool mode_pressed = mode_down && !mode_was_down;
        bool color_pressed = color_down && !color_was_down;
        mode_was_down = mode_down;
        color_was_down = color_down;
        bool serial_request = getchar_timeout_us(0) == 'm'; // check the key without waiting

        bool changed = false;
        if (mode_pressed || serial_request) {
            // switch views once the EEPROM save checks out
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
            frame_ready = false; // wait for a frame with the new settings
            show_controls(show_spectrum, band_mix_on);
        }

        if (!dma_channel_is_busy(audio_dma_channel)) {
            stats = finish_audio_block(&frame, show_spectrum, band_mix_on);
            frame_ready = true;
        }

        // leave time to refresh the matrix between serial prints
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
