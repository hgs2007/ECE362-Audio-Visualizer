#include "rgb_matrix.h"

#include "hardware/clocks.h"
#include "hardware/pio.h"
#include "pico/stdlib.h"
#include "rgb_matrix.pio.h"

const int RGB_BASE_GPIO = 27;
const int RGB_PIN_COUNT = 6; // three color pins for each of the two rows
const int CLOCK_GPIO = 33;
const int ROW_BASE_GPIO = 34;
const int ROW_PIN_COUNT = 4; // four address bits select one of 16 row pairs
const int LATCH_GPIO = 38;
const int OE_GPIO = 39;
const int GPIO_BASE = 16;
const int PIO_CLOCK_HZ = 2000000;
const int ROW_ON_CYCLES = 40; // 40 steps at 0.5 us per step is 20 us of light
const int ROW_PAIRS = RGB_MATRIX_HEIGHT / 2; // 32 rows split into 16 pairs
const int RGB_BITS_PER_PIXEL = 3; // one bit per color channel
const int COLUMN_PHASES = 4; // show one out of every four columns per scan

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

    // start the 32-pin PIO window at GP16 so it covers GP16 through GP47
    if(pio_set_gpio_base(pio0, GPIO_BASE) != PICO_OK)
    {
        return false;
    }

    // grab two state machines, false lets us check for a free slot ourselves
    shift_sm = pio_claim_unused_sm(pio0, false);
    row_sm = pio_claim_unused_sm(pio0, false);
    if(shift_sm < 0 || row_sm < 0)
    {
        return false;
    }

    int columns_offset = pio_add_program(pio0, &rgb_matrix_columns_program); // load instructions and get their start address
    int row_offset = pio_add_program(pio0, &rgb_matrix_row_program); // both programs fit in the same PIO block
    if(columns_offset < 0 || row_offset < 0)
    {
        return false;
    }
    float clock_divider = (float)clock_get_hz(clk_sys) / PIO_CLOCK_HZ; // at 150 MHz, divide by 75 to run PIO at 2 MHz

    // shift the RGB bits on GP27-GP32 and clock them in on GP33
    pio_sm_config shift_config = rgb_matrix_columns_program_get_default_config(columns_offset);
    sm_config_set_out_pins(&shift_config, RGB_BASE_GPIO, RGB_PIN_COUNT);
    sm_config_set_sideset_pins(&shift_config, CLOCK_GPIO); // the 'side' value drives CLK
    sm_config_set_out_shift(&shift_config, true, false, 32); // low bits first, use pull in PIO
    sm_config_set_clkdiv(&shift_config, clock_divider);
    if(pio_sm_init(pio0, shift_sm, columns_offset, &shift_config) != PICO_OK)
    {
        return false;
    }
    // set the six color pins and the clock pin as outputs
    if(pio_sm_set_consecutive_pindirs(pio0, shift_sm, RGB_BASE_GPIO,
                                      RGB_PIN_COUNT + 1, true) != PICO_OK)
    {
        return false;
    }

    // pick a row pair with A-D, latch the pixels, then light the rows
    pio_sm_config row_config = rgb_matrix_row_program_get_default_config(row_offset);
    sm_config_set_out_pins(&row_config, ROW_BASE_GPIO, ROW_PIN_COUNT);
    sm_config_set_sideset_pins(&row_config, LATCH_GPIO); // 'side' controls LAT and the next pin, OE
    sm_config_set_out_shift(&row_config, true, false, 32); // read the low bits first for the row address
    sm_config_set_clkdiv(&row_config, clock_divider);
    if(pio_sm_init(pio0, row_sm, row_offset, &row_config) != PICO_OK)
    {
        return false;
    }
    // set the four address pins plus LAT and OE as outputs
    if(pio_sm_set_consecutive_pindirs(pio0, row_sm, ROW_BASE_GPIO,
                                      ROW_PIN_COUNT + 2, true) != PICO_OK)
    {
        return false;
    }

    // start with OE high and LAT low before handing the pins to PIO
    // use 64-bit masks so shifting 1 to bit 38 or 39 still fits
    uint64_t oe_mask = (uint64_t)1 << OE_GPIO;
    uint64_t latch_mask = (uint64_t)1 << LATCH_GPIO;
    pio_sm_set_pins_with_mask64(pio0, row_sm, oe_mask, oe_mask | latch_mask); // touch both pins but only set OE high

    for(int pin = RGB_BASE_GPIO; pin <= OE_GPIO; ++pin)
    {
        pio_gpio_init(pio0, pin); // hand this GPIO over to PIO now that the outputs are ready
    }
    // clear old done flags before running the two programs
    pio_interrupt_clear(pio0, shift_sm);
    pio_interrupt_clear(pio0, row_sm);
    pio_sm_set_enabled(pio0, shift_sm, true);
    pio_sm_set_enabled(pio0, row_sm, true);
    return true;
}

// wait for the PIO done flag and clear it for the next row
static void wait_for_pio(int sm)
{
    while(!pio_interrupt_get(pio0, sm))
    {
        tight_loop_contents(); // keep checking while the PIO program runs on its own
    }
    pio_interrupt_clear(pio0, sm);
}

// pack the upper and lower pixels into one 6-bit column
static uint32_t column_bits(int x, int row, uint8_t pixels[][RGB_MATRIX_WIDTH])
{
    // x % 4 gives the remainder, so phase 0 shows columns 0, 4, 8 and so on
    // blank the other columns this pass to limit how many LEDs are on together
    if(x % COLUMN_PHASES != column_phase)
    {
        return 0;
    }
    // each address picks this row and the one 16 below it
    uint32_t upper = pixels[row][x]; // row 3 is in the top half
    uint32_t lower = pixels[row + ROW_PAIRS][x]; // row 3 + 16 is its partner in the bottom half
    return upper | (lower << RGB_BITS_PER_PIXEL); // upper RGB in bits 0-2, lower RGB in bits 3-5
}

// scan all 16 row pairs once, the main loop keeps calling this
void rgb_matrix_refresh(uint8_t pixels[][RGB_MATRIX_WIDTH])
{
    for(int row = 0; row < ROW_PAIRS; ++row)
    {
        for(int x = 0; x < RGB_MATRIX_WIDTH; x += 2)
        {
            uint32_t pair = column_bits(x, row, pixels); // first column in bits 0-5
            pair |= column_bits(x + 1, row, pixels) << RGB_PIN_COUNT; // shift the next column over six bits so it doesn't overlap
            pio_sm_put_blocking(pio0, shift_sm, pair); // send both columns to the PIO queue, wait if it's full
        }
        wait_for_pio(shift_sm); // wait for all 64 columns before latching

        uint32_t on_count = ROW_ON_CYCLES - 1; // 39 down through 0 gives 40 steps with the LEDs on
        uint32_t row_command = (on_count << ROW_PIN_COUNT) | row; // move the 39 up four bits to make room for the address
        pio_sm_put_blocking(pio0, row_sm, row_command); // tell the row program what to show and for how long
        wait_for_pio(row_sm); // wait for those rows to go dark before moving on
    }
    column_phase = (column_phase + 1) % COLUMN_PHASES; // cycle through 0, 1, 2, 3, then back to 0
}
