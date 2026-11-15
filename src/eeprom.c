#include "eeprom.h"

#include <stdint.h>

#include "hardware/i2c.h"
#include "pico/stdlib.h"

enum {
    EEPROM_SDA_GPIO = 4,
    EEPROM_SCL_GPIO = 5,
    EEPROM_I2C_ADDR = 0x50,
    EEPROM_I2C_HZ = 100000,
    I2C_TIMEOUT_US = 10000,
    EEPROM_WRITE_MS = 5,
    VIEW_SLOT = 0,
    COLOR_SLOT = 2, // Slot 1 was used for gain.
    SAVED_SPECTRUM = 1,
    SAVED_BAND_MIX = 3, // Same byte that the earlier firmware wrote.
};

// Set up I2C0 on the two pins wired to the EEPROM.
void eeprom_init(void)
{
    i2c_init(i2c0, EEPROM_I2C_HZ);
    gpio_set_function(EEPROM_SDA_GPIO, GPIO_FUNC_I2C);
    gpio_set_function(EEPROM_SCL_GPIO, GPIO_FUNC_I2C);
}

// Send the address inside the EEPROM, then read the bytes starting there.
static bool eeprom_read(uint8_t slot, uint8_t *data, int length)
{
    const uint8_t address[] = {0, slot}; // The EEPROM uses two address bytes.

    // Send the slot address first. 'true' keeps the bus open for the repeated start.
    int bytes_written = i2c_write_timeout_us(i2c0, EEPROM_I2C_ADDR, address, sizeof(address),
                                             true, I2C_TIMEOUT_US);
    int bytes_read = 0;
    if (bytes_written == (int)sizeof(address)) {
        bytes_read = i2c_read_timeout_us(i2c0, EEPROM_I2C_ADDR, data, length, false, I2C_TIMEOUT_US);
    }
    if (bytes_read != length) {
        i2c_init(i2c0, EEPROM_I2C_HZ); // Reset the controller for the next try.
        return false;
    }

    return true;
}

// Read the two choices on startup. False flags give us Waveform and RGB by default.
// The return value tells main whether it found a saved view.
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

// Write one byte and read it back so we know the save worked.
static bool eeprom_write_byte(uint8_t slot, uint8_t value)
{
    const uint8_t packet[] = {0, slot, value}; // Address high, address low, then data.
    int bytes_written = i2c_write_timeout_us(i2c0, EEPROM_I2C_ADDR, packet, sizeof(packet),
                                             false, I2C_TIMEOUT_US);
    if (bytes_written != (int)sizeof(packet)) {
        i2c_init(i2c0, EEPROM_I2C_HZ);
        return false;
    }

    // The 24AA32AF can take up to 5 ms to finish programming the byte.
    sleep_ms(EEPROM_WRITE_MS);
    uint8_t saved;
    return eeprom_read(slot, &saved, 1) && saved == value;
}

// Save the view when button 21 switches it. Zero means Waveform.
bool eeprom_save_view(bool show_spectrum)
{
    return eeprom_write_byte(VIEW_SLOT, show_spectrum ? SAVED_SPECTRUM : 0);
}

// Save the color when button 26 switches it. Zero means regular RGB.
bool eeprom_save_color(bool band_mix_on)
{
    return eeprom_write_byte(COLOR_SLOT, band_mix_on ? SAVED_BAND_MIX : 0);
}
