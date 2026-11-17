#include "eeprom.h"

#include <stdint.h>

#include "hardware/i2c.h"
#include "pico/stdlib.h"

#define VIEW_SLOT 0
#define COLOR_SLOT 2 // byte 1 used to hold gain

static const int EEPROM_SDA_GPIO = 4;
static const int EEPROM_SCL_GPIO = 5;
static const uint8_t EEPROM_I2C_ADDR = 0x50; // 7-bit address of the chip on the bus
static const int EEPROM_I2C_HZ = 100000; // standard mode
static const int I2C_TIMEOUT_US = 10000;
static const int EEPROM_WRITE_MS = 5;
static const uint8_t SAVED_SPECTRUM = 1;
static const uint8_t SAVED_BAND_MIX = 3; // keep the value from older saves

// set up I2C0 on GP4 and GP5, both lines have external pull-ups
void eeprom_init(void)
{
    i2c_init(i2c0, EEPROM_I2C_HZ);
    gpio_set_function(EEPROM_SDA_GPIO, GPIO_FUNC_I2C);
    gpio_set_function(EEPROM_SCL_GPIO, GPIO_FUNC_I2C);
}

// send the memory address, then read from that spot
static bool eeprom_read(uint16_t slot, uint8_t *data, int length)
{
    const uint8_t address[] = {slot >> 8, slot & 0xff}; // high byte first

    // true skips STOP so the next call can send a repeated START
    int bytes_written = i2c_write_timeout_us(i2c0, EEPROM_I2C_ADDR, address, sizeof(address),
                                            true, I2C_TIMEOUT_US);
    int bytes_read = 0;
    if (bytes_written == (int)sizeof(address)) {
        // false sends STOP after the last byte
        bytes_read = i2c_read_timeout_us(i2c0, EEPROM_I2C_ADDR, data, length, false, I2C_TIMEOUT_US);
    }
    if (bytes_read != length) {
        i2c_init(i2c0, EEPROM_I2C_HZ); // reset I2C so we can try again later
        return false;
    }

    return true;
}

// start with Waveform and RGB and check for saved choices
// return true if the saved view is valid
bool eeprom_load_choices(bool *show_spectrum, bool *band_mix_on)
{
    *show_spectrum = false;
    *band_mix_on = false;
    uint8_t saved[COLOR_SLOT + 1];
    if (!eeprom_read(VIEW_SLOT, saved, sizeof(saved))) {
        return false;
    }

    *show_spectrum = saved[VIEW_SLOT] == SAVED_SPECTRUM;
    *band_mix_on = saved[COLOR_SLOT] == SAVED_BAND_MIX;
    return saved[VIEW_SLOT] == 0 || *show_spectrum;
}

// save one byte and read it back to check it
static bool eeprom_write_byte(uint16_t slot, uint8_t value)
{
    const uint8_t packet[] = {slot >> 8, slot & 0xff, value}; // high byte, low byte, data
    // the EEPROM starts saving when it sees STOP
    int bytes_written = i2c_write_timeout_us(i2c0, EEPROM_I2C_ADDR, packet, sizeof(packet),
                                            false, I2C_TIMEOUT_US);
    if (bytes_written != (int)sizeof(packet)) {
        i2c_init(i2c0, EEPROM_I2C_HZ);
        return false;
    }

    // give the chip its 5 ms to finish the write
    sleep_ms(EEPROM_WRITE_MS);
    uint8_t saved;
    return eeprom_read(slot, &saved, 1) && saved == value;
}

// save 0 for Waveform or 1 for Spectrum
bool eeprom_save_view(bool show_spectrum)
{
    return eeprom_write_byte(VIEW_SLOT, show_spectrum ? SAVED_SPECTRUM : 0);
}

// save 0 for RGB or 3 for Band Mix
bool eeprom_save_color(bool band_mix_on)
{
    return eeprom_write_byte(COLOR_SLOT, band_mix_on ? SAVED_BAND_MIX : 0);
}
