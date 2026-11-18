#ifndef EEPROM_H
#define EEPROM_H

#include <stdbool.h>
#include <stdint.h>

#define SENSITIVITY_LEVEL_COUNT 3 // saved levels are 0 (Low), 1 (Medium), and 2 (High)
#define SENSITIVITY_DEFAULT 1 // Medium, used when nothing valid is saved

// set up SDA, SCL, and the I2C clock
void eeprom_init(void);
// fill the three choices through the pointers, return true for a valid saved view
bool eeprom_load_choices(bool *show_spectrum, bool *band_mix_on, uint8_t *sensitivity);
// each save returns true once the byte reads back correctly
bool eeprom_save_view(bool show_spectrum);
bool eeprom_save_sensitivity(uint8_t sensitivity);
bool eeprom_save_color(bool band_mix_on);

#endif
