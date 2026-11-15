#ifndef SETTINGS_H
#define SETTINGS_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    DISPLAY_MODE_WAVEFORM = 0,
    DISPLAY_MODE_SPECTRUM = 1,
} DisplayMode;

typedef enum {
    PALETTE_RGB = 0,
    PALETTE_BAND_MIX = 3, // Keep the value already saved by older firmware.
} Palette;

// These numbers are also the EEPROM byte addresses.
typedef enum {
    SETTING_MODE = 0,
    SETTING_COLOR = 2, // Byte 1 was used for display gain.
} SettingField;

typedef struct {
    DisplayMode mode;
    Palette palette;
} Settings;

void settings_init(void);
bool settings_load(Settings *settings); // True if the mode byte was restored.
bool settings_save(SettingField field, uint8_t value);

#endif
