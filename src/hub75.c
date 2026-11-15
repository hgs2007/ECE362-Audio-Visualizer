#include "hub75.h"

#include "hardware/clocks.h"
#include "hardware/pio.h"
#include "pico/stdlib.h"
#include "hub75.pio.h"

enum {
    RGB_BASE_GPIO = 27,
    RGB_PIN_COUNT = 6,
    CLOCK_GPIO = 33,
    ROW_BASE_GPIO = 34,
    ROW_PIN_COUNT = 4,
    LATCH_GPIO = 38,
    OE_GPIO = 39,
    GPIO_BASE = 16,
    PIO_CLOCK_HZ = 2000000,
    ROW_ON_CYCLES = 40, // 20 us at the PIO clock rate.
    ROW_PAIRS = HUB75_HEIGHT / 2,
    RGB_BITS_PER_PIXEL = 3,
    COLUMN_PHASES = 4,
};

static uint columns_sm;
static uint row_sm;
static unsigned column_phase;

void hub75_init(void)
{
    // OE is active-low. Keep the panel blank while we set the pins up.
    gpio_init(OE_GPIO);
    gpio_put(OE_GPIO, true);
    gpio_set_dir(OE_GPIO, GPIO_OUT);

    // PIO gets 32 pins at a time. The GP16-GP47 window includes all our panel pins.
    hard_assert(pio_set_gpio_base(pio0, GPIO_BASE) == PICO_OK);
    columns_sm = pio_claim_unused_sm(pio0, true);
    row_sm = pio_claim_unused_sm(pio0, true);
    int columns_offset = pio_add_program(pio0, &hub75_columns_program);
    int row_offset = pio_add_program(pio0, &hub75_row_program);
    hard_assert(columns_offset >= 0 && row_offset >= 0);
    float clock_divider = (float)clock_get_hz(clk_sys) / PIO_CLOCK_HZ;

    // Like the PIO part of Lab 6, data goes out through OUT and side-set drives CLK.
    pio_sm_config columns_config = hub75_columns_program_get_default_config(columns_offset);
    sm_config_set_out_pins(&columns_config, RGB_BASE_GPIO, RGB_PIN_COUNT);
    sm_config_set_sideset_pins(&columns_config, CLOCK_GPIO);
    sm_config_set_out_shift(&columns_config, true, false, 32); // Low bits first, manual pull.
    sm_config_set_clkdiv(&columns_config, clock_divider);
    hard_assert(pio_sm_init(pio0, columns_sm, columns_offset, &columns_config) == PICO_OK);
    hard_assert(pio_sm_set_consecutive_pindirs(pio0, columns_sm, RGB_BASE_GPIO,
                                              RGB_PIN_COUNT + 1, true) == PICO_OK);

    // The other state machine sets A-D, pulses LAT, and turns the row pair on briefly.
    pio_sm_config row_config = hub75_row_program_get_default_config(row_offset);
    sm_config_set_out_pins(&row_config, ROW_BASE_GPIO, ROW_PIN_COUNT);
    sm_config_set_sideset_pins(&row_config, LATCH_GPIO);
    sm_config_set_out_shift(&row_config, true, false, 32);
    sm_config_set_clkdiv(&row_config, clock_divider);
    hard_assert(pio_sm_init(pio0, row_sm, row_offset, &row_config) == PICO_OK);
    hard_assert(pio_sm_set_consecutive_pindirs(pio0, row_sm, ROW_BASE_GPIO,
                                              ROW_PIN_COUNT + 2, true) == PICO_OK);
    pio_sm_set_pins_with_mask64(pio0, row_sm, 1ull << OE_GPIO,
                                (1ull << OE_GPIO) | (1ull << LATCH_GPIO));

    for (uint pin = RGB_BASE_GPIO; pin <= OE_GPIO; ++pin) {
        pio_gpio_init(pio0, pin);
    }
    pio_interrupt_clear(pio0, columns_sm);
    pio_interrupt_clear(pio0, row_sm);
    pio_sm_set_enabled(pio0, columns_sm, true);
    pio_sm_set_enabled(pio0, row_sm, true);
}

static void wait_for_state_machine(uint sm)
{
    while (!pio_interrupt_get(pio0, sm)) {
        tight_loop_contents();
    }
    pio_interrupt_clear(pio0, sm);
}

static uint32_t column_bits(unsigned x, unsigned row, const Hub75Frame *frame)
{
    // Light every fourth column this pass to limit current. Change the offset next pass.
    if (x % COLUMN_PHASES != column_phase) {
        return 0;
    }
    // One address selects row y in the top half and y + 16 in the bottom half.
    uint32_t upper = frame->pixels[row][x];
    uint32_t lower = frame->pixels[row + ROW_PAIRS][x];
    return upper | (lower << RGB_BITS_PER_PIXEL);
}

void hub75_refresh(const Hub75Frame *frame)
{
    // Same idea as the multiplexed display in Lab 3. Scan it over and over.
    for (unsigned row = 0; row < ROW_PAIRS; ++row) {
        for (unsigned x = 0; x < HUB75_WIDTH; x += 2) {
            // First column goes in bits 0-5 and the second in bits 6-11.
            uint32_t pair = column_bits(x, row, frame) |
                            (column_bits(x + 1, row, frame) << RGB_PIN_COUNT);
            pio_sm_put_blocking(pio0, columns_sm, pair);
        }
        wait_for_state_machine(columns_sm); // Wait for the last CLK pulse before latching.

        // Low four bits pick the rows. The rest set how long OE stays low, like a PWM pulse.
        uint32_t row_command = ((ROW_ON_CYCLES - 1u) << ROW_PIN_COUNT) | row;
        pio_sm_put_blocking(pio0, row_sm, row_command);
        wait_for_state_machine(row_sm);
    }
    column_phase = (column_phase + 1) % COLUMN_PHASES;
}
