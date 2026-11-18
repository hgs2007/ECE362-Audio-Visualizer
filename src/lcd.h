#ifndef LCD_H
#define LCD_H

#define LCD_COLUMNS 16 // characters on one line

// set up SPI and send the power-up commands
void lcd_init(void);
// write the top and bottom lines, padding either one with spaces
void lcd_show_lines(const char *top, const char *bottom);

#endif
