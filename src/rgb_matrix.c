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

// Set up the panel pins and start the two PIO state machines
void rgb_matrix_init(void)
{
    // Keep OE high so the panel stays dark while we set up the pins
    gpio_init(OE_GPIO);
    gpio_put(OE_GPIO, true);
    gpio_set_dir(OE_GPIO, GPIO_OUT);

    // Start the PIO pin window at GP16 so it can reach GP39
    hard_assert(pio_set_gpio_base(pio0, GPIO_BASE) == PICO_OK);
    shift_sm = pio_claim_unused_sm(pio0, true);
    row_sm = pio_claim_unused_sm(pio0, true);
    int columns_offset = pio_add_program(pio0, &rgb_matrix_columns_program);
    int row_offset = pio_add_program(pio0, &rgb_matrix_row_program);
    hard_assert(columns_offset >= 0 && row_offset >= 0);
    float clock_divider = (float)clock_get_hz(clk_sys) / PIO_CLOCK_HZ;

    // The shift program puts colors on GP27-GP32 and clocks them in with GP33
    pio_sm_config shift_config = rgb_matrix_columns_program_get_default_config(columns_offset);
    sm_config_set_out_pins(&shift_config, RGB_BASE_GPIO, RGB_PIN_COUNT);
    sm_config_set_sideset_pins(&shift_config, CLOCK_GPIO);
    sm_config_set_out_shift(&shift_config, true, false, 32); // Low bits first, wait for pull
    sm_config_set_clkdiv(&shift_config, clock_divider);
    hard_assert(pio_sm_init(pio0, shift_sm, columns_offset, &shift_config) == PICO_OK);
    hard_assert(pio_sm_set_consecutive_pindirs(pio0, shift_sm, RGB_BASE_GPIO,
                                              RGB_PIN_COUNT + 1, true) == PICO_OK);

    // The other state machine sets A-D, pulses LAT, and turns the row pair on briefly
    pio_sm_config row_config = rgb_matrix_row_program_get_default_config(row_offset);
    sm_config_set_out_pins(&row_config, ROW_BASE_GPIO, ROW_PIN_COUNT);
    sm_config_set_sideset_pins(&row_config, LATCH_GPIO);
    sm_config_set_out_shift(&row_config, true, false, 32);
    sm_config_set_clkdiv(&row_config, clock_divider);
    hard_assert(pio_sm_init(pio0, row_sm, row_offset, &row_config) == PICO_OK);
    hard_assert(pio_sm_set_consecutive_pindirs(pio0, row_sm, ROW_BASE_GPIO,
                                              ROW_PIN_COUNT + 2, true) == PICO_OK);
    // OE and LAT are above GP31, so these masks need 64 bits
    uint64_t oe_mask = UINT64_C(1) << OE_GPIO;
    uint64_t latch_mask = UINT64_C(1) << LATCH_GPIO;
    pio_sm_set_pins_with_mask64(pio0, row_sm, oe_mask, oe_mask | latch_mask);

    for (int pin = RGB_BASE_GPIO; pin <= OE_GPIO; ++pin) {
        pio_gpio_init(pio0, pin);
    }
    pio_interrupt_clear(pio0, shift_sm);
    pio_interrupt_clear(pio0, row_sm);
    pio_sm_set_enabled(pio0, shift_sm, true);
    pio_sm_set_enabled(pio0, row_sm, true);
}

// Wait for the flag set by the PIO irq instruction, then clear it for the next row
static void wait_for_pio(int sm)
{
    while (!pio_interrupt_get(pio0, sm)) {
        tight_loop_contents();
    }
    pio_interrupt_clear(pio0, sm);
}

// Pack the upper and lower pixels into the six bits that go to the panel for this column
static uint32_t column_bits(int x, int row, const struct RgbMatrixFrame *frame)
{
    // Light every fourth column this pass and use a different starting column next pass
    if (x % COLUMN_PHASES != column_phase) {
        return 0;
    }
    // One address selects row y in the top half and y + 16 in the bottom half
    uint32_t upper = frame->pixels[row][x];
    uint32_t lower = frame->pixels[row + ROW_PAIRS][x];
    return upper | (lower << RGB_BITS_PER_PIXEL);
}

// Scan all 16 row pairs once, then the main loop calls this again to keep the image visible
void rgb_matrix_refresh(const struct RgbMatrixFrame *frame)
{
    for (int row = 0; row < ROW_PAIRS; ++row) {
        for (int x = 0; x < RGB_MATRIX_WIDTH; x += 2) {
            // First column goes in bits 0-5 and the second in bits 6-11
            uint32_t pair = column_bits(x, row, frame);
            pair |= column_bits(x + 1, row, frame) << RGB_PIN_COUNT;
            pio_sm_put_blocking(pio0, shift_sm, pair);
        }
        wait_for_pio(shift_sm); // All 64 columns have to be in before we latch them

        // Low four bits pick the rows, and the upper bits hold the OE countdown
        uint32_t row_command = ((uint32_t)(ROW_ON_CYCLES - 1) << ROW_PIN_COUNT) | (uint32_t)row;
        pio_sm_put_blocking(pio0, row_sm, row_command);
        wait_for_pio(row_sm);
    }
    column_phase = (column_phase + 1) % COLUMN_PHASES;
}
