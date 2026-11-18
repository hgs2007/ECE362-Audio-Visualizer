#ifndef EEPROM_H
#define EEPROM_H

#include <stdbool.h>

// set up SDA, SCL, and the I2C clock
void eeprom_init(void);
// fill the two choices through the pointers, return true for a valid saved view
bool eeprom_load_choices(bool *show_spectrum, bool *band_mix_on);
// each save returns true once the byte reads back correctly
bool eeprom_save_view(bool show_spectrum);
bool eeprom_save_color(bool band_mix_on);

#endif
