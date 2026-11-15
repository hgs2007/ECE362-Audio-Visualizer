#include "settings.h"

#include "hardware/i2c.h"
#include "pico/stdlib.h"

enum {
    EEPROM_SDA_GPIO = 4,
    EEPROM_SCL_GPIO = 5,
    EEPROM_I2C_ADDR = 0x50,
    EEPROM_I2C_HZ = 100000,
    I2C_TIMEOUT_US = 10000,
    EEPROM_WRITE_MS = 5,
};

static bool valid_setting(SettingField field, uint8_t value)
{
    if (field == SETTING_MODE) {
        return value == DISPLAY_MODE_WAVEFORM || value == DISPLAY_MODE_SPECTRUM;
    }
    return field == SETTING_COLOR && (value == PALETTE_RGB || value == PALETTE_BAND_MIX);
}

void settings_init(void)
{
    i2c_init(i2c0, EEPROM_I2C_HZ);
    gpio_set_function(EEPROM_SDA_GPIO, GPIO_FUNC_I2C);
    gpio_set_function(EEPROM_SCL_GPIO, GPIO_FUNC_I2C);
}

static bool eeprom_read(uint16_t slot, uint8_t *data, size_t length)
{
    const uint8_t address[] = {slot >> 8, slot & 0xff};

    // Send the slot address first. 'true' keeps the bus open for the repeated start.
    int bytes_written = i2c_write_timeout_us(i2c0, EEPROM_I2C_ADDR, address, sizeof(address),
                                             true, I2C_TIMEOUT_US);
    int bytes_read = 0;
    if (bytes_written == (int)sizeof(address)) {
        bytes_read = i2c_read_timeout_us(i2c0, EEPROM_I2C_ADDR, data, length, false, I2C_TIMEOUT_US);
    }
    if (bytes_read != (int)length) {
        i2c_init(i2c0, EEPROM_I2C_HZ); // Reset the controller for the next try.
        return false;
    }

    return true;
}

bool settings_load(Settings *settings)
{
    *settings = (Settings){
        .mode = DISPLAY_MODE_WAVEFORM,
        .palette = PALETTE_RGB,
    };
    uint8_t saved[SETTING_COLOR + 1];
    if (!eeprom_read(SETTING_MODE, saved, sizeof(saved))) {
        return false;
    }

    bool mode_saved = valid_setting(SETTING_MODE, saved[SETTING_MODE]);
    if (mode_saved) {
        settings->mode = (DisplayMode)saved[SETTING_MODE];
    }
    // Blank or retired color values use RGB.
    if (valid_setting(SETTING_COLOR, saved[SETTING_COLOR])) {
        settings->palette = (Palette)saved[SETTING_COLOR];
    }
    return mode_saved;
}

bool settings_save(SettingField field, uint8_t value)
{
    if (!valid_setting(field, value)) {
        return false;
    }

    // All our slots are below 256, so the high address byte is zero.
    const uint8_t packet[] = {0, (uint8_t)field, value};
    int bytes_written = i2c_write_timeout_us(i2c0, EEPROM_I2C_ADDR, packet, sizeof(packet),
                                             false, I2C_TIMEOUT_US);
    if (bytes_written != (int)sizeof(packet)) {
        i2c_init(i2c0, EEPROM_I2C_HZ);
        return false;
    }

    // The 24AA32AF can take up to 5 ms to finish programming the byte.
    sleep_ms(EEPROM_WRITE_MS);
    // Check what actually got stored before we say the save worked.
    uint8_t saved;
    return eeprom_read((uint16_t)field, &saved, 1) && saved == value;
}
