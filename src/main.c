#include <stdio.h>
#include <string.h>

#include "hardware/adc.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "pico/stdlib.h"
#include "hub75.h"
#include "lcd.h"
#include "settings.h"
#include "spectrum.h"

enum {
    MIC_GPIO = 40, // MAX4466 OUT is wired to GP40.
    MODE_BUTTON_GPIO = 21,
    COLOR_BUTTON_GPIO = 26,
    STATUS_LED_GPIO = 22,
    REPORT_PAUSE_MS = 250,
    FIRST_ADC_GPIO = 40,
    LAST_ADC_GPIO = 47,
    AUDIO_SAMPLE_RATE_HZ = 16000,
    AUDIO_BLOCK_SAMPLES = SPECTRUM_SAMPLE_COUNT,
    SAMPLES_PER_COLUMN = AUDIO_BLOCK_SAMPLES / HUB75_WIDTH,
    ADC_COUNTS_PER_ROW = 32,
    SPECTRUM_NOISE_FLOOR = 6,
    SPECTRUM_COUNTS_PER_ROW = 8,
    ADC_MAX_COUNT = 4095,
    BASS_BAR_COUNT = 4,
    TREBLE_FIRST_BAR = 12, // Zero-based index in the 16-bar display.
    WAVE_MID_ROWS = 3,
    WAVE_PEAK_ROWS = 8,
};

typedef struct {
    unsigned min;
    unsigned max;
    unsigned mean;
} AudioStats;

static uint16_t audio_samples[AUDIO_BLOCK_SAMPLES];
static uint audio_dma_channel;

enum { BASS, MIDRANGE, TREBLE, BAND_COUNT };

static const uint8_t band_colors[BAND_COUNT] = {
    HUB75_RED, HUB75_GREEN, HUB75_BLUE,
};

static void mic_init(void)
{
    adc_init();
    adc_gpio_init(MIC_GPIO);
    // The Proton's ADC channels start at GP40, so GP40 is channel 0.
    adc_select_input(MIC_GPIO - FIRST_ADC_GPIO);
    adc_fifo_setup(true,   // Store samples in the FIFO.
                   true,   // Let the FIFO request a DMA transfer.
                   1,      // Request as soon as one sample is ready.
                   false,  // Leave the error flag out of the sample.
                   false); // Keep all 12 bits of the reading.

    // Free-running samples are spaced by 1 + divider ADC clock cycles.
    adc_set_clkdiv((float)clock_get_hz(clk_adc) / AUDIO_SAMPLE_RATE_HZ - 1.0f);

    // Same ADC FIFO and DREQ as Lab 4. Here we fill an array for each frame.
    audio_dma_channel = dma_claim_unused_channel(true);
    dma_channel_config config = dma_channel_get_default_config(audio_dma_channel);
    channel_config_set_transfer_data_size(&config, DMA_SIZE_16); // 12 bits fit in a halfword
    channel_config_set_read_increment(&config, false); // Read the same FIFO register.
    channel_config_set_write_increment(&config, true); // Move to the next array slot.
    channel_config_set_dreq(&config, DREQ_ADC);
    dma_channel_configure(audio_dma_channel, &config, audio_samples, &adc_hw->fifo,
                          AUDIO_BLOCK_SAMPLES, true);
    adc_run(true);
}

static const char *mode_name(DisplayMode mode)
{
    return mode == DISPLAY_MODE_WAVEFORM ? "Waveform" : "Spectrum";
}

static const char *color_name(Palette palette)
{
    return palette == PALETTE_BAND_MIX ? "Band Mix" : "RGB";
}

static void show_controls(const Settings *settings)
{
    char top[LCD_COLUMNS + 1];
    snprintf(top, sizeof(top), "21 View:%s", mode_name(settings->mode));
    const char *bottom = settings->palette == PALETTE_BAND_MIX ? "26 Color:Mix" : "26 Color:RGB";
    lcd_show_lines(top, bottom);
}

static bool toggle_setting(Settings *settings, SettingField field)
{
    Settings next = *settings;
    uint8_t value;
    const char *name;
    if (field == SETTING_MODE) {
        next.mode = next.mode == DISPLAY_MODE_WAVEFORM ? DISPLAY_MODE_SPECTRUM : DISPLAY_MODE_WAVEFORM;
        value = next.mode;
        name = mode_name(next.mode);
    } else {
        next.palette = next.palette == PALETTE_RGB ? PALETTE_BAND_MIX : PALETTE_RGB;
        value = next.palette;
        name = color_name(next.palette);
    }

    // Keep the old value if the EEPROM write or readback failed.
    if (!settings_save(field, value)) {
        puts("EEPROM save could not be verified at address 0x50.");
        return false;
    }
    *settings = next;
    printf("Saved %s to EEPROM.\n", name);
    return true;
}

static unsigned band_for_bar(unsigned bar)
{
    if (bar < BASS_BAR_COUNT) {
        return BASS;
    }
    if (bar >= TREBLE_FIRST_BAR) {
        return TREBLE;
    }
    return MIDRANGE;
}

static uint8_t mix_band_colors(const float amplitudes[SPECTRUM_BAR_COUNT])
{
    float peaks[BAND_COUNT] = {0};
    float strongest = 0.0f;
    for (unsigned bar = 0; bar < SPECTRUM_BAR_COUNT; ++bar) {
        unsigned band = band_for_bar(bar);
        if (amplitudes[bar] > peaks[band]) {
            peaks[band] = amplitudes[bar];
        }
        if (amplitudes[bar] > strongest) {
            strongest = amplitudes[bar];
        }
    }

    // Include any band that's above the noise floor and at least half the strongest band.
    uint8_t mixed = 0;
    for (unsigned band = 0; band < BAND_COUNT; ++band) {
        if (peaks[band] > SPECTRUM_NOISE_FLOOR && peaks[band] >= strongest * 0.5f) {
            mixed |= band_colors[band]; // OR turns on more than one LED channel at a time.
        }
    }
    return mixed ? mixed : HUB75_BLUE; // Keep a visible trace in a quiet room.
}

static void draw_waveform(Hub75Frame *frame, unsigned mean, uint8_t mixed)
{
    // Use one sample out of each group of four and center the trace at row 16.
    for (unsigned x = 0; x < HUB75_WIDTH; ++x) {
        int centered = (int)audio_samples[x * SAMPLES_PER_COLUMN] - (int)mean;
        int offset = centered / ADC_COUNTS_PER_ROW;
        int row = HUB75_HEIGHT / 2 - offset; // Row 0 is at the top.
        if (row < 0) {
            row = 0;
        } else if (row >= HUB75_HEIGHT) {
            row = HUB75_HEIGHT - 1;
        }
        uint8_t color = mixed;
        if (!color) {
            // Use distance above or below the center for the three amplitude colors.
            int distance = offset < 0 ? -offset : offset;
            color = HUB75_BLUE;
            if (distance >= WAVE_PEAK_ROWS) {
                color = HUB75_RED;
            } else if (distance >= WAVE_MID_ROWS) {
                color = HUB75_GREEN;
            }
        }
        frame->pixels[row][x] = color;
    }
}

static void draw_spectrum(Hub75Frame *frame, const float amplitudes[SPECTRUM_BAR_COUNT], uint8_t mixed)
{
    // Each bar has three colored columns and a one-column gap.
    const unsigned columns_per_bar = HUB75_WIDTH / SPECTRUM_BAR_COUNT;
    for (unsigned bar = 0; bar < SPECTRUM_BAR_COUNT; ++bar) {
        float amplitude = amplitudes[bar] - SPECTRUM_NOISE_FLOOR;
        if (amplitude <= 0.0f) {
            continue;
        }
        unsigned height = (unsigned)(amplitude / SPECTRUM_COUNTS_PER_ROW);
        if (height > HUB75_HEIGHT) {
            height = HUB75_HEIGHT;
        }
        uint8_t color = mixed ? mixed : band_colors[band_for_bar(bar)];
        unsigned first_column = bar * columns_per_bar;
        for (unsigned row = HUB75_HEIGHT - height; row < HUB75_HEIGHT; ++row) {
            for (unsigned x = first_column; x < first_column + columns_per_bar - 1; ++x) {
                frame->pixels[row][x] = color;
            }
        }
    }
}

static AudioStats finish_audio_block(Hub75Frame *frame, const Settings *settings)
{
    // The loop already saw DMA finish. The SDK wait also makes the buffer safe to read.
    dma_channel_wait_for_finish_blocking(audio_dma_channel);
    adc_run(false);
    adc_fifo_drain(); // Drop any samples that arrived after the array filled.

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
    unsigned mean = sum / AUDIO_BLOCK_SAMPLES; // The mic sits around half of 3.3 V at idle.

    memset(frame, 0, sizeof(*frame));
    bool mixing = settings->palette == PALETTE_BAND_MIX;
    float amplitudes[SPECTRUM_BAR_COUNT];
    if (settings->mode == DISPLAY_MODE_SPECTRUM || mixing) {
        spectrum_measure(audio_samples, mean, amplitudes);
    }
    uint8_t mixed = mixing ? mix_band_colors(amplitudes) : 0;
    if (settings->mode == DISPLAY_MODE_WAVEFORM) {
        draw_waveform(frame, mean, mixed);
    } else {
        draw_spectrum(frame, amplitudes, mixed);
    }

    // Point DMA back at the start of the array once we've used the old samples.
    dma_channel_transfer_to_buffer_now(audio_dma_channel, audio_samples, AUDIO_BLOCK_SAMPLES);
    adc_run(true);
    return (AudioStats){.min = min, .max = max, .mean = mean};
}

static void report_mic_level(AudioStats stats, DisplayMode mode)
{
    printf("mic GP%d: min=%u max=%u mean=%u peak_to_peak=%u mode=%s\n",
           MIC_GPIO, stats.min, stats.max, stats.mean, stats.max - stats.min, mode_name(mode));
}

int main(void)
{
    stdio_init_all(); // Use the debugger UART for printf and the 'm' key, like Lab 7.
    gpio_init(STATUS_LED_GPIO);
    gpio_set_dir(STATUS_LED_GPIO, GPIO_OUT);
    gpio_init(MODE_BUTTON_GPIO);
    gpio_set_dir(MODE_BUTTON_GPIO, GPIO_IN);
    gpio_init(COLOR_BUTTON_GPIO);
    gpio_set_dir(COLOR_BUTTON_GPIO, GPIO_IN);

    hub75_init();
    settings_init();
    Settings settings;
    bool restored = settings_load(&settings);
    printf("Mode: %s (%s). 21 switches view, 26 switches color.\n",
           mode_name(settings.mode), restored ? "loaded from EEPROM" : "default");
    printf("Color: %s.\n", color_name(settings.palette));
    lcd_init();
    show_controls(&settings);

    spectrum_init();
    mic_init();

    bool led_on = false;
    bool mode_was_down = gpio_get(MODE_BUTTON_GPIO);
    bool color_was_down = gpio_get(COLOR_BUTTON_GPIO);
    bool frame_ready = false;
    AudioStats stats = {0};
    static Hub75Frame frame; // Keep the pixel array off the stack.
    absolute_time_t next_report = get_absolute_time();
    while (true) {
        bool mode_down = gpio_get(MODE_BUTTON_GPIO);
        bool color_down = gpio_get(COLOR_BUTTON_GPIO);
        // Act once per new press, even if the button stays down.
        bool mode_pressed = mode_down && !mode_was_down;
        bool color_pressed = color_down && !color_was_down;
        mode_was_down = mode_down;
        color_was_down = color_down;
        bool serial_request = getchar_timeout_us(0) == 'm'; // Keep scanning if nobody types.

        bool changed = false;
        if (mode_pressed || serial_request) {
            changed = toggle_setting(&settings, SETTING_MODE);
        }
        if (color_pressed && toggle_setting(&settings, SETTING_COLOR)) {
            changed = true;
        }
        if (changed) {
            frame_ready = false; // Wait for a picture with the new settings.
            show_controls(&settings);
        }

        if (!dma_channel_is_busy(audio_dma_channel)) {
            stats = finish_audio_block(&frame, &settings);
            frame_ready = true;
        }

        // Limit UART prints so the matrix can keep refreshing between reports.
        if (frame_ready && time_reached(next_report)) {
            led_on = !led_on;
            gpio_put(STATUS_LED_GPIO, led_on);
            report_mic_level(stats, settings.mode);
            next_report = make_timeout_time_ms(REPORT_PAUSE_MS);
        }

        if (frame_ready) {
            hub75_refresh(&frame);
        } else {
            sleep_ms(1);
        }
    }
}
