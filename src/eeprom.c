#include "eeprom.h"

#include <stdint.h>

#include "hardware/i2c.h"
#include "pico/stdlib.h"

#define VIEW_SLOT 0 // first byte in the chip holds the view
#define SENSITIVITY_SLOT 1 // second byte holds the mic sensitivity level
#define COLOR_SLOT 2 // third byte holds the color

const int EEPROM_SDA_GPIO = 4;
const int EEPROM_SCL_GPIO = 5;
const uint8_t EEPROM_I2C_ADDR = 0x50; // 7-bit address of the chip on the bus
const int EEPROM_I2C_HZ = 100000; // standard mode
const int I2C_TIMEOUT_US = 10000; // stop waiting for a transfer after 10 ms
const int EEPROM_WRITE_MS = 5; // time the chip needs to finish a write
const uint8_t SAVED_SPECTRUM = 1; // saved view is 0 for Waveform or 1 for Spectrum
const uint8_t SAVED_BAND_MIX = 3; // keep the value from older saves

// set up I2C0 on GP4 and GP5, both lines have external pull-ups
void eeprom_init(void)
{
    i2c_init(i2c0, EEPROM_I2C_HZ);
    gpio_set_function(EEPROM_SDA_GPIO, GPIO_FUNC_I2C); // data goes both ways on SDA
    gpio_set_function(EEPROM_SCL_GPIO, GPIO_FUNC_I2C); // use SCL for the clock from the Proton
}

// send the memory address, then read from that spot
static bool eeprom_read(uint16_t slot, uint8_t *data, int length)
{
    // the chip wants a two-byte memory address, so slot 2 is sent as 00 02
    const uint8_t address[] = {slot >> 8, slot & 0xff}; // shift the high byte down, then mask to get the low byte
    const int address_size = sizeof(address); // both address bytes have to get through

    // point the chip at the address without ending the transfer, so we can switch to reading
    int bytes_written = i2c_write_timeout_us(i2c0, EEPROM_I2C_ADDR, address, address_size,
                                            true, I2C_TIMEOUT_US);
    int bytes_read = 0; // stays at zero if we couldn't send the address
    if(bytes_written == address_size)
    {
        // false sends STOP after the last byte
        bytes_read = i2c_read_timeout_us(i2c0, EEPROM_I2C_ADDR, data, length, false, I2C_TIMEOUT_US);
    }
    if(bytes_read != length)
    {
        i2c_init(i2c0, EEPROM_I2C_HZ); // reset I2C so we can try again later
        return false;
    }

    return true;
}

// start with Waveform, Medium sensitivity, and RGB, then check for saved choices
// return true if the saved view is valid
bool eeprom_load_choices(bool *show_spectrum, bool *band_mix_on, uint8_t *sensitivity)
{
    *show_spectrum = false; // default to Waveform if the read fails
    *sensitivity = SENSITIVITY_DEFAULT; // default to Medium if the read fails
    *band_mix_on = false; // default to RGB if the read fails
    uint8_t saved[COLOR_SLOT + 1]; // read bytes 0, 1, and 2 in one transfer
    if(!eeprom_read(VIEW_SLOT, saved, sizeof(saved)))
    {
        return false;
    }

    *show_spectrum = saved[VIEW_SLOT] == SAVED_SPECTRUM;
    *band_mix_on = saved[COLOR_SLOT] == SAVED_BAND_MIX;
    if(saved[SENSITIVITY_SLOT] < SENSITIVITY_LEVEL_COUNT) // a blank chip reads 0xFF, so keep the default
    {
        *sensitivity = saved[SENSITIVITY_SLOT];
    }
    return saved[VIEW_SLOT] == 0 || *show_spectrum; // only 0 and 1 count as a saved view
}

// save one byte and read it back to check it
static bool eeprom_write_byte(uint16_t slot, uint8_t value)
{
    const uint8_t packet[] = {slot >> 8, slot & 0xff, value}; // same address split as the read, then the value to store
    const int packet_size = sizeof(packet); // two address bytes plus the value
    // the EEPROM starts saving when it sees STOP
    int bytes_written = i2c_write_timeout_us(i2c0, EEPROM_I2C_ADDR, packet, packet_size,
                                            false, I2C_TIMEOUT_US);
    if(bytes_written != packet_size)
    {
        i2c_init(i2c0, EEPROM_I2C_HZ);
        return false;
    }

    // give the chip its 5 ms to finish the write
    sleep_ms(EEPROM_WRITE_MS);
    uint8_t saved;
    return eeprom_read(slot, &saved, 1) && saved == value; // compare only if we got a byte back
}

// save 0 for Waveform or 1 for Spectrum
bool eeprom_save_view(bool show_spectrum)
{
    return eeprom_write_byte(VIEW_SLOT, show_spectrum ? SAVED_SPECTRUM : 0);
}

// save 0 for Low, 1 for Medium, or 2 for High
bool eeprom_save_sensitivity(uint8_t sensitivity)
{
    if(sensitivity >= SENSITIVITY_LEVEL_COUNT)
    {
        return false; // don't store a level the load code would ignore
    }
    return eeprom_write_byte(SENSITIVITY_SLOT, sensitivity);
}

// save 0 for RGB or 3 for Band Mix
bool eeprom_save_color(bool band_mix_on)
{
    return eeprom_write_byte(COLOR_SLOT, band_mix_on ? SAVED_BAND_MIX : 0);
}
