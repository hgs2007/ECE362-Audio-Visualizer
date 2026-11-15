#include "settings.h"

#include "hardware/i2c.h"
#include "pico/stdlib.h"

enum {
    EEPROM_SDA_GPIO = 4,
    EEPROM_SCL_GPIO = 5,
    EEPROM_ADDRESS = 0x50,
    EEPROM_I2C_HZ = 100000,
    I2C_TIMEOUT_US = 10000,
    EEPROM_WRITE_MS = 5,
    MODE_ADDRESS = 0x0000,
};

void settings_init(void)
{
    i2c_init(i2c0, EEPROM_I2C_HZ);
    gpio_set_function(EEPROM_SDA_GPIO, GPIO_FUNC_I2C);
    gpio_set_function(EEPROM_SCL_GPIO, GPIO_FUNC_I2C);
}

bool settings_load(DisplayMode *mode)
{
    const uint8_t address[] = {MODE_ADDRESS >> 8, MODE_ADDRESS & 0xff};
    uint8_t value;

    // Set the EEPROM address, then read a byte using a repeated start.
    if (i2c_write_timeout_us(i2c0, EEPROM_ADDRESS, address, sizeof(address),
                             true, I2C_TIMEOUT_US) != sizeof(address) ||
        i2c_read_timeout_us(i2c0, EEPROM_ADDRESS, &value, 1,
                            false, I2C_TIMEOUT_US) != 1) {
        i2c_init(i2c0, EEPROM_I2C_HZ);
        return false;
    }

    if (value != DISPLAY_MODE_WAVEFORM && value != DISPLAY_MODE_SPECTRUM) {
        return false;
    }

    *mode = (DisplayMode)value;
    return true;
}

bool settings_save(DisplayMode mode)
{
    if (mode != DISPLAY_MODE_WAVEFORM && mode != DISPLAY_MODE_SPECTRUM) {
        return false;
    }

    const uint8_t write[] = {MODE_ADDRESS >> 8, MODE_ADDRESS & 0xff, (uint8_t)mode};
    if (i2c_write_timeout_us(i2c0, EEPROM_ADDRESS, write, sizeof(write),
                             false, I2C_TIMEOUT_US) != sizeof(write)) {
        i2c_init(i2c0, EEPROM_I2C_HZ);
        return false;
    }

    // The 24AA32AF can take up to 5 ms to finish programming the byte.
    sleep_ms(EEPROM_WRITE_MS);
    DisplayMode saved_mode;
    return settings_load(&saved_mode) && saved_mode == mode;
}
