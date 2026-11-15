#ifndef SETTINGS_H
#define SETTINGS_H

#include <stdbool.h>

typedef enum {
    DISPLAY_MODE_WAVEFORM = 0,
    DISPLAY_MODE_SPECTRUM = 1,
} DisplayMode;

void settings_init(void);
bool settings_load(DisplayMode *mode);
bool settings_save(DisplayMode mode);

#endif
