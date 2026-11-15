#ifndef EEPROM_H
#define EEPROM_H

#include <stdbool.h>

void eeprom_init(void);
bool eeprom_load_choices(bool *show_spectrum, bool *band_mix_on);
bool eeprom_save_view(bool show_spectrum);
bool eeprom_save_color(bool band_mix_on);

#endif
