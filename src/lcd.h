#ifndef LCD_H
#define LCD_H

enum { LCD_COLUMNS = 16 };

void lcd_init(void);
void lcd_show_lines(const char *top, const char *bottom);

#endif
