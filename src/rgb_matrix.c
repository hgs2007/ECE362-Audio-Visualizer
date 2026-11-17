#include "rgb_matrix.h"

#include "hardware/clocks.h"
#include "hardware/pio.h"
#include "pico/stdlib.h"
#include "rgb_matrix.pio.h"

static const int RGB_BASE_GPIO = 27;
static const int RGB_PIN_COUNT = 6;
static const int CLOCK_GPIO = 33;
static const int ROW_BASE_GPIO = 34;
static const int ROW_PIN_COUNT = 4;
static const int LATCH_GPIO = 38;
static const int OE_GPIO = 39;
static const int GPIO_BASE = 16;
static const int PIO_CLOCK_HZ = 2000000;
static const int ROW_ON_CYCLES = 40; // 20 us at 2 MHz
static const int ROW_PAIRS = RGB_MATRIX_HEIGHT / 2;
static const int RGB_BITS_PER_PIXEL = 3;
static const int COLUMN_PHASES = 4;

static int shift_sm;
static int row_sm;
static int column_phase;

// set up the panel pins and start both PIO programs
bool rgb_matrix_init(void)
{
    // keep the panel dark during setup, OE is active low
    gpio_init(OE_GPIO);
    gpio_put(OE_GPIO, true);
    gpio_set_dir(OE_GPIO, GPIO_OUT);

    // start the PIO pin window at GP16 so it can reach GP39
    if (pio_set_gpio_base(pio0, GPIO_BASE) != PICO_OK) {
        return false;
    }

    // grab two state machines, false lets us check for a free slot ourselves
    shift_sm = pio_claim_unused_sm(pio0, false);
    row_sm = pio_claim_unused_sm(pio0, false);
    if (shift_sm < 0 || row_sm < 0) {
        return false;
    }

    int columns_offset = pio_add_program(pio0, &rgb_matrix_columns_program);
    int row_offset = pio_add_program(pio0, &rgb_matrix_row_program);
    if (columns_offset < 0 || row_offset < 0) {
        return false;
    }
    float clock_divider = (float)clock_get_hz(clk_sys) / PIO_CLOCK_HZ;

    // shift the RGB bits on GP27-GP32 and clock them in on GP33
    pio_sm_config shift_config = rgb_matrix_columns_program_get_default_config(columns_offset);
    sm_config_set_out_pins(&shift_config, RGB_BASE_GPIO, RGB_PIN_COUNT);
    sm_config_set_sideset_pins(&shift_config, CLOCK_GPIO);
    sm_config_set_out_shift(&shift_config, true, false, 32); // low bits first, use pull in PIO
    sm_config_set_clkdiv(&shift_config, clock_divider);
    if (pio_sm_init(pio0, shift_sm, columns_offset, &shift_config) != PICO_OK) {
        return false;
    }
    if (pio_sm_set_consecutive_pindirs(pio0, shift_sm, RGB_BASE_GPIO,
                                       RGB_PIN_COUNT + 1, true) != PICO_OK) {
        return false;
    }

    // pick a row pair with A-D, latch the pixels, then light the rows
    pio_sm_config row_config = rgb_matrix_row_program_get_default_config(row_offset);
    sm_config_set_out_pins(&row_config, ROW_BASE_GPIO, ROW_PIN_COUNT);
    sm_config_set_sideset_pins(&row_config, LATCH_GPIO);
    sm_config_set_out_shift(&row_config, true, false, 32);
    sm_config_set_clkdiv(&row_config, clock_divider);
    if (pio_sm_init(pio0, row_sm, row_offset, &row_config) != PICO_OK) {
        return false;
    }
    if (pio_sm_set_consecutive_pindirs(pio0, row_sm, ROW_BASE_GPIO,
                                       ROW_PIN_COUNT + 2, true) != PICO_OK) {
        return false;
    }

    // start with OE high and LAT low before handing the pins to PIO
    // use 64-bit masks so we can reach GP38 and GP39
    uint64_t oe_mask = (uint64_t)1 << OE_GPIO;
    uint64_t latch_mask = (uint64_t)1 << LATCH_GPIO;
    pio_sm_set_pins_with_mask64(pio0, row_sm, oe_mask, oe_mask | latch_mask);

    for (int pin = RGB_BASE_GPIO; pin <= OE_GPIO; ++pin) {
        pio_gpio_init(pio0, pin);
    }
    pio_interrupt_clear(pio0, shift_sm);
    pio_interrupt_clear(pio0, row_sm);
    pio_sm_set_enabled(pio0, shift_sm, true);
    pio_sm_set_enabled(pio0, row_sm, true);
    return true;
}

// wait for the PIO done flag and clear it for the next row
static void wait_for_pio(int sm)
{
    while (!pio_interrupt_get(pio0, sm)) {
        tight_loop_contents();
    }
    pio_interrupt_clear(pio0, sm);
}

// pack the upper and lower pixels into one 6-bit column
static uint32_t column_bits(int x, int row, const struct RgbMatrixFrame *frame)
{
    // light every fourth column this pass to limit the current draw
    if (x % COLUMN_PHASES != column_phase) {
        return 0;
    }
    // each address picks this row and the one 16 below it
    uint32_t upper = frame->pixels[row][x];
    uint32_t lower = frame->pixels[row + ROW_PAIRS][x];
    return upper | (lower << RGB_BITS_PER_PIXEL);
}

// scan all 16 row pairs once, the main loop keeps calling this
void rgb_matrix_refresh(const struct RgbMatrixFrame *frame)
{
    for (int row = 0; row < ROW_PAIRS; ++row) {
        for (int x = 0; x < RGB_MATRIX_WIDTH; x += 2) {
            // first column in bits 0-5, second in bits 6-11
            uint32_t pair = column_bits(x, row, frame);
            pair |= column_bits(x + 1, row, frame) << RGB_PIN_COUNT;
            pio_sm_put_blocking(pio0, shift_sm, pair);
        }
        wait_for_pio(shift_sm); // wait for all 64 columns before latching

        uint32_t on_count = ROW_ON_CYCLES - 1; // the PIO loop counts down through zero
        uint32_t row_command = (on_count << ROW_PIN_COUNT) | row; // send the row address in the low bits
        pio_sm_put_blocking(pio0, row_sm, row_command);
        wait_for_pio(row_sm);
    }
    column_phase = (column_phase + 1) % COLUMN_PHASES; // start one column over next pass
}
