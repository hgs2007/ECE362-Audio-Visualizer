#include <stdio.h>

#include "hardware/adc.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "pico/stdlib.h"
#include "hub75.h"
#include "settings.h"

enum {
    MIC_GPIO = 40, // MAX4466 OUT is wired to GP40.
    MODE_BUTTON_GPIO = 21,
    STATUS_LED_GPIO = 22,
    REPORT_PAUSE_MS = 250,
    FIRST_ADC_GPIO = 40,
    LAST_ADC_GPIO = 47,
    AUDIO_SAMPLE_RATE_HZ = 16000,
    AUDIO_BLOCK_SAMPLES = 256,
    SAMPLES_PER_COLUMN = AUDIO_BLOCK_SAMPLES / HUB75_WIDTH,
    ADC_COUNTS_PER_ROW = 32,
    ADC_MAX_COUNT = 4095,
};

_Static_assert(MIC_GPIO >= FIRST_ADC_GPIO && MIC_GPIO <= LAST_ADC_GPIO,
               "MIC_GPIO must be a Proton ADC pin (GP40 through GP47)");

typedef struct {
    unsigned min;
    unsigned max;
    unsigned mean;
} AudioStats;

static uint16_t audio_samples[AUDIO_BLOCK_SAMPLES];
static uint audio_dma_channel;

static void mic_init(void)
{
    adc_init();
    adc_gpio_init(MIC_GPIO);
    adc_select_input(MIC_GPIO - FIRST_ADC_GPIO);
    adc_fifo_setup(true, true, 1, false, false);

    // Free-running samples are spaced by 1 + divider ADC clock cycles.
    adc_set_clkdiv((float)clock_get_hz(clk_adc) / AUDIO_SAMPLE_RATE_HZ - 1.0f);

    audio_dma_channel = dma_claim_unused_channel(true);
    dma_channel_config config = dma_channel_get_default_config(audio_dma_channel);
    channel_config_set_transfer_data_size(&config, DMA_SIZE_16);
    channel_config_set_read_increment(&config, false);
    channel_config_set_write_increment(&config, true);
    channel_config_set_dreq(&config, DREQ_ADC);
    dma_channel_configure(audio_dma_channel, &config, audio_samples, &adc_hw->fifo,
                          AUDIO_BLOCK_SAMPLES, true);
    adc_run(true);
}

static const char *mode_name(DisplayMode mode)
{
    return mode == DISPLAY_MODE_WAVEFORM ? "Waveform" : "Spectrum";
}

static AudioStats finish_audio_block(uint8_t waveform_rows[HUB75_WIDTH])
{
    // DMA has finished this block. Discard samples that arrived after it filled.
    dma_channel_wait_for_finish_blocking(audio_dma_channel);
    adc_run(false);
    adc_fifo_drain();

    unsigned min = ADC_MAX_COUNT;
    unsigned max = 0;
    unsigned sum = 0;

    for (unsigned i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
        unsigned sample = audio_samples[i];
        if (sample < min) {
            min = sample;
        }
        if (sample > max) {
            max = sample;
        }
        sum += sample;
    }
    unsigned mean = sum / AUDIO_BLOCK_SAMPLES;

    // Use one sample out of each group of four and center the trace at row 16.
    for (unsigned x = 0; x < HUB75_WIDTH; ++x) {
        int centered = (int)audio_samples[x * SAMPLES_PER_COLUMN] - (int)mean;
        int row = HUB75_HEIGHT / 2 - centered / ADC_COUNTS_PER_ROW;
        if (row < 0) {
            row = 0;
        } else if (row >= HUB75_HEIGHT) {
            row = HUB75_HEIGHT - 1;
        }
        waveform_rows[x] = (uint8_t)row;
    }

    dma_channel_transfer_to_buffer_now(audio_dma_channel, audio_samples, AUDIO_BLOCK_SAMPLES);
    adc_run(true);
    return (AudioStats){min, max, mean};
}

static void report_mic_level(AudioStats stats, DisplayMode mode)
{
    printf("mic GP%d: min=%u max=%u mean=%u peak_to_peak=%u mode=%s\n",
           MIC_GPIO, stats.min, stats.max, stats.mean, stats.max - stats.min, mode_name(mode));
}

int main(void)
{
    stdio_init_all();
    gpio_init(STATUS_LED_GPIO);
    gpio_set_dir(STATUS_LED_GPIO, GPIO_OUT);
    gpio_init(MODE_BUTTON_GPIO);
    gpio_set_dir(MODE_BUTTON_GPIO, GPIO_IN);

    hub75_init();
    settings_init();
    DisplayMode mode = DISPLAY_MODE_WAVEFORM;
    bool restored = settings_load(&mode);
    printf("Mode: %s (%s). Press button 21 or m to change and save.\n",
           mode_name(mode), restored ? "loaded from EEPROM" : "default");

    mic_init();

    bool led_on = false;
    bool button_was_pressed = gpio_get(MODE_BUTTON_GPIO);
    bool have_audio_block = false;
    AudioStats stats = {0};
    uint8_t waveform_rows[HUB75_WIDTH];
    absolute_time_t next_report = get_absolute_time();
    while (true) {
        bool button_pressed = gpio_get(MODE_BUTTON_GPIO);
        bool new_press = button_pressed && !button_was_pressed;
        button_was_pressed = button_pressed;
        bool serial_request = getchar_timeout_us(0) == 'm';

        if (new_press || serial_request) {
            DisplayMode next_mode = mode == DISPLAY_MODE_WAVEFORM
                ? DISPLAY_MODE_SPECTRUM : DISPLAY_MODE_WAVEFORM;
            if (settings_save(next_mode)) {
                mode = next_mode;
                printf("Saved %s to EEPROM.\n", mode_name(mode));
            } else {
                puts("EEPROM save could not be verified at address 0x50.");
            }
        }

        if (!dma_channel_is_busy(audio_dma_channel)) {
            stats = finish_audio_block(waveform_rows);
            have_audio_block = true;
        }

        if (have_audio_block && time_reached(next_report)) {
            led_on = !led_on;
            gpio_put(STATUS_LED_GPIO, led_on);
            report_mic_level(stats, mode);
            next_report = make_timeout_time_ms(REPORT_PAUSE_MS);
        }

        if (have_audio_block && mode == DISPLAY_MODE_WAVEFORM) {
            hub75_refresh_waveform(waveform_rows);
        } else {
            sleep_ms(1);
        }
    }
}
