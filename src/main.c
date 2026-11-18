#include <stdio.h> 

#include "hardware/adc.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "pico/stdlib.h"
#include "eeprom.h"
#include "rgb_matrix.h"
#include "lcd.h"
#include "spectrum.h"

#define AUDIO_BLOCK_SAMPLES SPECTRUM_SAMPLE_COUNT // both views start with the same 256 samples
#define SAMPLES_PER_COLUMN (AUDIO_BLOCK_SAMPLES / RGB_MATRIX_WIDTH) // 256 / 64 gives a step of 4 through the samples
#define BAND_COUNT 3 // bass, mids, treble

const int MIC_GPIO = 40; // mic output from the MAX4466
const int SELECT_BUTTON_GPIO = 21; // picks which setting is selected
const int CHANGE_BUTTON_GPIO = 26; // changes the selected setting
const int STATUS_LED_GPIO = 22;
const int REPORT_PAUSE_MS = 250; // print mic stats at most four times a second
const int FIRST_ADC_GPIO = 40; // start counting ADC inputs at GP40
const int AUDIO_SAMPLE_RATE_HZ = 16000; // 256 samples at 16 kHz is 16 ms of sound
const int ADC_COUNTS_PER_ROW = 32; // move the waveform one row per 32 counts from center
const int SPECTRUM_NOISE_FLOOR = 6; // ignore small FFT readings from a quiet mic
const int SPECTRUM_COUNTS_PER_ROW = 8; // grow a bar by one row per 8 counts above the floor
const int ADC_MAX_COUNT = 4095; // 12 bits gives 4096 possible values, starting at 0
const int BASS_BAR_COUNT = 4; // bars 0 through 3 are bass
const int TREBLE_FIRST_BAR = 12; // bars are numbered 0 to 15
const int WAVE_MID_ROWS = 3; // switch from blue to green this far from center
const int WAVE_PEAK_ROWS = 8; // switch from green to red this far from center

// these arrays stay around between calls and are only used in this file
static uint16_t audio_samples[AUDIO_BLOCK_SAMPLES]; // holds the 12-bit mic readings from DMA
static uint8_t pixels[RGB_MATRIX_HEIGHT][RGB_MATRIX_WIDTH]; // our picture, indexed by row then column
static int audio_dma_channel; // channel number the SDK gives us

// keep the stats from the last full block for the serial log
static int audio_min;
static int audio_max;
static int audio_mean;

// use these numbers as indexes into the peak and color arrays
const int BASS = 0;
const int MIDRANGE = 1;
const int TREBLE = 2;

// the number of the selected setting is also the EEPROM byte it's saved in
enum {
    SETTING_VIEW, // byte 0
    SETTING_SENSITIVITY, // byte 1
    SETTING_COLOR, // byte 2
    SETTING_COUNT
};

// sensitivity scales how far the waveform and bars move for the same sound
// Medium keeps the picture the way it looked before sensitivity existed
const float SENSITIVITY_GAINS[SENSITIVITY_LEVEL_COUNT] = {0.5f, 1.0f, 2.0f};
const char *const SENSITIVITY_NAMES[SENSITIVITY_LEVEL_COUNT] = {"Low", "Medium", "High"};

const uint8_t band_colors[BAND_COUNT] = {
    RGB_MATRIX_RED, RGB_MATRIX_GREEN, RGB_MATRIX_BLUE,
};

// set up the mic ADC and let DMA fill the sample array
static void mic_init(void)
{
    adc_init(); // power up the ADC
    adc_gpio_init(MIC_GPIO); // set GP40 up for analog input
    // since ADC channels start at gp40, it's channel 0
    adc_select_input(MIC_GPIO - FIRST_ADC_GPIO);
    adc_fifo_setup(true, // put samples in the FIFO
                   true, // let the FIFO ask DMA for a transfer
                   1, // ask as soon as there's one sample
                   false, // leave out the error bit
                   false); // keep all 12 bits

    // at 48 MHz, 48000000 / 16000 gives 3000 ADC clocks per sample
    // the hardware uses divider + 1, so we send 2999 in that case
    adc_set_clkdiv((float)clock_get_hz(clk_adc) / AUDIO_SAMPLE_RATE_HZ - 1.0f);

    // one DMA transfer per sample
    audio_dma_channel = dma_claim_unused_channel(true); // reserve a channel for the mic
    dma_channel_config config = dma_channel_get_default_config(audio_dma_channel); // start with the SDK defaults
    channel_config_set_transfer_data_size(&config, DMA_SIZE_16); // each array slot holds 16 bits
    channel_config_set_read_increment(&config, false); // always read the same FIFO register
    channel_config_set_write_increment(&config, true); // write to the next spot in the array
    channel_config_set_dreq(&config, DREQ_ADC); // wait for an ADC request before copying each sample
    // give DMA the destination, the source register, and the number of samples to copy
    dma_channel_configure(audio_dma_channel, &config, audio_samples, &adc_hw->fifo,
                          AUDIO_BLOCK_SAMPLES, true); // arm DMA now so it's ready for the first sample
    adc_run(true); // start making ADC samples
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

// show the selected setting and its value, with a reminder of what each button does
static void show_controls(int selected, bool show_spectrum, uint8_t sensitivity, bool band_mix_on)
{
    char top[LCD_COLUMNS + 1]; // one extra byte for the null at the end of the string
    if(selected == SETTING_VIEW)
    {
        snprintf(top, sizeof(top), "View:%s", mode_name(show_spectrum)); // limit the text to the array size
    }
    else if(selected == SETTING_SENSITIVITY)
    {
        snprintf(top, sizeof(top), "Sens:%s", SENSITIVITY_NAMES[sensitivity]);
    }
    else
    {
        snprintf(top, sizeof(top), "Color:%s", color_name(band_mix_on));
    }
    lcd_show_lines(top, "21 Next 26 Edit");
}

// first 4 bars are bass, next 8 are mids, last 4 are treble
static int band_for_bar(int bar)
{
    if(bar < BASS_BAR_COUNT)
    {
        return BASS;
    }
    if(bar >= TREBLE_FIRST_BAR)
    {
        return TREBLE;
    }
    return MIDRANGE;
}

// use the tallest bar in each band to decide which colors to mix
static uint8_t mix_band_colors(const float amplitudes[SPECTRUM_BAR_COUNT])
{
    float peaks[BAND_COUNT] = {0}; // one best reading for each of the three bands
    float strongest = 0.0f; // best reading across all 16 bars
    for(int bar = 0; bar < SPECTRUM_BAR_COUNT; ++bar)
    {
        int band = band_for_bar(bar);
        if(amplitudes[bar] > peaks[band])
        {
            peaks[band] = amplitudes[bar];
        }
        if(amplitudes[bar] > strongest)
        {
            strongest = amplitudes[bar];
        }
    }

    // if the biggest peak is 40, another band needs at least 20 to join it
    // the noise floor also stops quiet input from turning on the band colors
    uint8_t mixed = 0;
    for(int band = 0; band < BAND_COUNT; ++band)
    {
        if(peaks[band] > SPECTRUM_NOISE_FLOOR && peaks[band] >= strongest * 0.5f)
        {
            mixed |= band_colors[band]; // keep the old bits too, so red | green gives yellow
        }
    }
    return mixed ? mixed : RGB_MATRIX_BLUE; // use blue when it's too quiet to mix
}

// plot every fourth sample so 256 samples fit in 64 columns
static void draw_waveform(uint8_t mixed, float gain)
{
    for(int x = 0; x < RGB_MATRIX_WIDTH; ++x)
    {
        // columns 0, 1, 2 use samples 0, 4, 8 and so on
        int centered = audio_samples[x * SAMPLES_PER_COLUMN] - audio_mean; // take out the average so the wave sits around zero
        // 64 counts above average gives a 2-row offset at gain 1, and gain 2 doubles that to 4 rows
        int offset = (int)(centered * gain) / ADC_COUNTS_PER_ROW;
        int row = RGB_MATRIX_HEIGHT / 2 - offset; // positive sound swings go up, so row 16 becomes row 14
        // keep the row index within 0 through 31 even for loud peaks
        if(row < 0)
        {
            row = 0;
        }
        else if(row >= RGB_MATRIX_HEIGHT)
        {
            row = RGB_MATRIX_HEIGHT - 1;
        }
        uint8_t color = mixed; // use one color for the whole trace in Band Mix
        if(!color)
        {
            // color depends on distance from center in RGB mode
            int distance = offset < 0 ? -offset : offset; // use the same color at +3 and -3 rows
            color = RGB_MATRIX_BLUE;
            if(distance >= WAVE_PEAK_ROWS)
            {
                color = RGB_MATRIX_RED;
            }
            else if(distance >= WAVE_MID_ROWS)
            {
                color = RGB_MATRIX_GREEN;
            }
        }
        pixels[row][x] = color; // light one dot in this column
    }
}

// draw the 16 frequency bars from the bottom up
static void draw_spectrum(const float amplitudes[SPECTRUM_BAR_COUNT], uint8_t mixed)
{
    // 64 columns / 16 bars leaves 4 columns each, including a gap
    const int columns_per_bar = RGB_MATRIX_WIDTH / SPECTRUM_BAR_COUNT;
    for(int bar = 0; bar < SPECTRUM_BAR_COUNT; ++bar)
    {
        float amplitude = amplitudes[bar] - SPECTRUM_NOISE_FLOOR; // a reading of 30 leaves 24 after subtracting 6
        if(amplitude <= 0.0f)
        {
            continue; // leave the bar blank when the reading is at or below the floor
        }
        int height = amplitude / SPECTRUM_COUNTS_PER_ROW; // 24 / 8 gives a 3-row bar
        if(height > RGB_MATRIX_HEIGHT)
        {
            height = RGB_MATRIX_HEIGHT; // cap the bar at 32 rows
        }
        uint8_t color = mixed ? mixed : band_colors[band_for_bar(bar)]; // use the mix if it's on, otherwise use this band's color
        int first_column = bar * columns_per_bar; // bar 2 starts at column 8
        // 32 - 3 starts a 3-row bar at row 29, then we fill down to row 31
        for(int row = RGB_MATRIX_HEIGHT - height; row < RGB_MATRIX_HEIGHT; ++row)
        {
            // stop one column early for the gap, so bar 2 uses columns 8, 9, and 10
            for(int x = first_column; x < first_column + columns_per_bar - 1; ++x)
            {
                pixels[row][x] = color;
            }
        }
    }
}

// find the smallest and largest readings for the log and the average for drawing
static void measure_audio_block(void)
{
    audio_min = ADC_MAX_COUNT; // start high so any smaller reading can replace it
    audio_max = 0; // start low so any bigger reading can replace it
    int sum = 0;

    for(int i = 0; i < AUDIO_BLOCK_SAMPLES; ++i)
    {
        int sample = audio_samples[i];
        if(sample < audio_min)
        {
            audio_min = sample;
        }
        if(sample > audio_max)
        {
            audio_max = sample;
        }
        sum += sample; // add this sample to the total for the average
    }
    audio_mean = sum / AUDIO_BLOCK_SAMPLES; // average voltage in ADC counts, used as our zero
}

// clear the old dots and bars before drawing again
static void clear_frame(void)
{
    for(int row = 0; row < RGB_MATRIX_HEIGHT; ++row)
    {
        for(int x = 0; x < RGB_MATRIX_WIDTH; ++x)
        {
            pixels[row][x] = 0; // all three color bits off
        }
    }
}

// use the samples we just got, then start another capture
static void finish_audio_block(bool show_spectrum, bool band_mix_on, uint8_t sensitivity)
{
    float gain = SENSITIVITY_GAINS[sensitivity]; // 0.5, 1, or 2 times the usual size
    // wait before reading the array so we have the full block from DMA
    dma_channel_wait_for_finish_blocking(audio_dma_channel);
    adc_run(false); // stop sampling while we use this block
    adc_fifo_drain(); // toss anything left in the queue so the next block starts fresh

    measure_audio_block();
    clear_frame();
    float amplitudes[SPECTRUM_BAR_COUNT]; // one strength value for each of the 16 bars
    // regular RGB waveform only needs the samples, Band Mix also needs their frequencies
    if(show_spectrum || band_mix_on)
    {
        spectrum_measure(audio_samples, audio_mean, amplitudes);
        // scale before drawing or mixing, so sensitivity also moves the noise floor and band colors
        for(int bar = 0; bar < SPECTRUM_BAR_COUNT; ++bar)
        {
            amplitudes[bar] *= gain;
        }
    }
    uint8_t mixed = band_mix_on ? mix_band_colors(amplitudes) : 0; // zero tells the drawing code to use regular RGB
    if(show_spectrum)
    {
        draw_spectrum(amplitudes, mixed);
    }
    else
    {
        draw_waveform(mixed, gain);
    }

    // set DMA back to the start of the array before turning the ADC back on
    dma_channel_transfer_to_buffer_now(audio_dma_channel, audio_samples, AUDIO_BLOCK_SAMPLES);
    adc_run(true);
}

// print the sample range so we can see if the mic is picking up sound
static void report_mic_level(bool show_spectrum)
{
    // peak to peak spans the whole wave, from the lowest sample to the highest
    printf("mic GP%d: min=%d max=%d mean=%d peak_to_peak=%d mode=%s\n",
           MIC_GPIO, audio_min, audio_max, audio_mean, audio_max - audio_min, mode_name(show_spectrum));
}

// save the opposite view and hand the new choice back to the main loop
static bool switch_view(bool show_spectrum)
{
    bool next_view = !show_spectrum; // true is Spectrum and false is Waveform
    if(eeprom_save_view(next_view))
    {
        printf("Saved %s to EEPROM.\n", mode_name(next_view));
        return next_view;
    }
    puts("Could not save the view to EEPROM at 0x50.");
    return show_spectrum; // keep the old choice if the save failed
}

// move to the next sensitivity level and wrap from High back to Low
static uint8_t switch_sensitivity(uint8_t sensitivity)
{
    uint8_t next_level = (sensitivity + 1) % SENSITIVITY_LEVEL_COUNT;
    if(eeprom_save_sensitivity(next_level))
    {
        printf("Saved sensitivity %s to EEPROM.\n", SENSITIVITY_NAMES[next_level]);
        return next_level;
    }
    puts("Could not save the sensitivity to EEPROM at 0x50.");
    return sensitivity; // keep the old level if the save failed
}

// same idea as switching views, but this writes the color slot
static bool switch_color(bool band_mix_on)
{
    bool next_color = !band_mix_on; // true is Band Mix and false is RGB
    if(eeprom_save_color(next_color))
    {
        printf("Saved %s to EEPROM.\n", color_name(next_color));
        return next_color;
    }
    puts("Could not save the color to EEPROM at 0x50.");
    return band_mix_on;
}

// start the hardware and keep the audio, buttons, and screen running
int main(void)
{
    stdio_init_all(); // use the debugger UART for logs and the 'm' key
    gpio_init(STATUS_LED_GPIO);
    gpio_set_dir(STATUS_LED_GPIO, GPIO_OUT); // we drive the light ourselves
    gpio_init(SELECT_BUTTON_GPIO);
    gpio_set_dir(SELECT_BUTTON_GPIO, GPIO_IN); // the button drives this pin, we just read it
    gpio_init(CHANGE_BUTTON_GPIO);
    gpio_set_dir(CHANGE_BUTTON_GPIO, GPIO_IN);

    if(!rgb_matrix_init())
    {
        puts("Could not start the RGB matrix.");
        return 1; // stop here if PIO setup failed so we never try to send a frame
    }
    eeprom_init();
    bool show_spectrum;
    bool band_mix_on;
    uint8_t sensitivity;
    // fill all three choices and report if the view was saved
    bool restored = eeprom_load_choices(&show_spectrum, &band_mix_on, &sensitivity);
    printf("Mode: %s (%s). 21 selects a setting, 26 changes it.\n",
           mode_name(show_spectrum), restored ? "loaded from EEPROM" : "default");
    printf("Sensitivity: %s.\n", SENSITIVITY_NAMES[sensitivity]);
    printf("Color: %s.\n", color_name(band_mix_on));
    int selected = SETTING_VIEW; // start on the first setting, byte 0
    lcd_init();
    show_controls(selected, show_spectrum, sensitivity, band_mix_on); // show the choices we started with

    spectrum_init(); // build the FFT tables once at startup
    mic_init(); // start filling the first sample block

    bool led_on = false;
    // read the starting levels so a button held at startup doesn't count as a new press
    bool select_was_down = gpio_get(SELECT_BUTTON_GPIO);
    bool change_was_down = gpio_get(CHANGE_BUTTON_GPIO);
    bool frame_ready = false; // the first block is still being collected
    absolute_time_t next_report = get_absolute_time(); // print as soon as a frame is ready
    while(true)
    {
        bool select_down = gpio_get(SELECT_BUTTON_GPIO);
        bool change_down = gpio_get(CHANGE_BUTTON_GPIO);
        // a press is low last time and high this time, so holding the button counts once
        bool select_pressed = select_down && !select_was_down;
        bool change_pressed = change_down && !change_was_down;
        select_was_down = select_down; // remember these levels for the next loop
        change_was_down = change_down;
        bool serial_request = getchar_timeout_us(0) == 'm'; // check the key without waiting

        // keep the old choices so we can tell which saves actually worked
        bool old_view = show_spectrum;
        uint8_t old_sensitivity = sensitivity;
        bool old_color = band_mix_on;
        bool selection_moved = false;
        // button 21 moves to the next setting: 0 View, 1 Sensitivity, 2 Color, then back to 0
        if(select_pressed)
        {
            selected = (selected + 1) % SETTING_COUNT;
            selection_moved = true;
            printf("Selected setting %d.\n", selected);
        }
        // button 26 changes whichever setting is selected
        if(change_pressed)
        {
            if(selected == SETTING_VIEW)
            {
                show_spectrum = switch_view(show_spectrum);
            }
            else if(selected == SETTING_SENSITIVITY)
            {
                sensitivity = switch_sensitivity(sensitivity);
            }
            else
            {
                band_mix_on = switch_color(band_mix_on);
            }
        }
        // the 'm' key still switches the view directly, whichever setting is selected
        if(serial_request)
        {
            show_spectrum = switch_view(show_spectrum);
        }
        bool choice_changed = show_spectrum != old_view || sensitivity != old_sensitivity || band_mix_on != old_color;
        if(choice_changed)
        {
            frame_ready = false; // stop refreshing the picture drawn with the old choices
        }
        // leave the LCD alone if nothing changed
        if(choice_changed || selection_moved)
        {
            show_controls(selected, show_spectrum, sensitivity, band_mix_on);
        }

        // once all 256 sample slots are filled, DMA goes idle
        if(!dma_channel_is_busy(audio_dma_channel))
        {
            finish_audio_block(show_spectrum, band_mix_on, sensitivity);
            frame_ready = true; // we can scan this picture until the next block finishes
        }

        // leave time to refresh the matrix between serial prints
        if(frame_ready && time_reached(next_report))
        {
            led_on = !led_on; // blink the status LED to show the loop is running
            gpio_put(STATUS_LED_GPIO, led_on);
            report_mic_level(show_spectrum);
            next_report = make_timeout_time_ms(REPORT_PAUSE_MS); // next print is 250 ms from now
        }

        if(frame_ready)
        {
            rgb_matrix_refresh(pixels); // scan the current picture again while DMA gets the next block
        }
        else
        {
            sleep_ms(1); // give DMA a little time if there's nothing to display yet
        }
    }
}
