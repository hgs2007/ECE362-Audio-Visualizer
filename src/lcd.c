#include "lcd.h"

#include "hardware/spi.h"
#include "pico/stdlib.h"

enum {
    LCD_CS_GPIO = 13,
    LCD_SCK_GPIO = 14,
    LCD_TX_GPIO = 15,
    LCD_SPI_HZ = 10000,
    LCD_DATA_BIT = 1 << 8,
    LCD_LINE_1 = 0x80,
    LCD_LINE_2 = 0xc0,
};

// Send one 9-bit SPI word like in Lab 6. Bit 8 marks a character.
static void lcd_send(uint16_t word)
{
    spi_write16_blocking(spi1, &word, 1);
}

// Set up SPI and send the startup commands for the two-line LCD.
void lcd_init(void)
{
    spi_init(spi1, LCD_SPI_HZ);
    spi_set_format(spi1, 9, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_set_function(LCD_CS_GPIO, GPIO_FUNC_SPI);
    gpio_set_function(LCD_SCK_GPIO, GPIO_FUNC_SPI);
    gpio_set_function(LCD_TX_GPIO, GPIO_FUNC_SPI);

    sleep_ms(50); // Give the LCD time to power up before sending commands.
    lcd_send(0x3c); // Function set for the two-line LCD.
    sleep_us(40);
    lcd_send(0x0c); // Display on, cursor and blink off.
    sleep_us(40);
    lcd_send(0x01); // Clear takes longer than the other commands.
    sleep_ms(2);
    lcd_send(0x06); // Move the cursor right after writing each character.
    sleep_us(40);
}

// Move the cursor to the start of a line and fill all 16 spaces.
static void lcd_write_line(uint16_t cursor_command, const char *text)
{
    lcd_send(cursor_command);
    for (int x = 0; x < LCD_COLUMNS; ++x) {
        // Fill the rest with spaces to erase letters from a longer previous value.
        char ch = ' ';
        if (*text != '\0') {
            ch = *text;
            text++;
        }
        lcd_send(LCD_DATA_BIT | (uint8_t)ch);
    }
}

// Replace both LCD lines with the latest button labels and choices.
void lcd_show_lines(const char *top, const char *bottom)
{
    lcd_write_line(LCD_LINE_1, top);
    lcd_write_line(LCD_LINE_2, bottom);
}
