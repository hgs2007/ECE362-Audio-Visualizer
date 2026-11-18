#include "lcd.h"

#include "hardware/spi.h"
#include "pico/stdlib.h"

static const int LCD_CS_GPIO = 13;
static const int LCD_SCK_GPIO = 14;
static const int LCD_TX_GPIO = 15;
static const int LCD_SPI_HZ = 10000; // clock the display at 10 kHz
static const uint16_t LCD_DATA_BIT = 1 << 8; // the ninth bit says the low eight bits are a character
static const uint16_t LCD_LINE_1 = 0x80; // go to text address 0x00 for the top line
static const uint16_t LCD_LINE_2 = 0xc0; // go to text address 0x40 for the bottom line

// send one SPI word, bit 8 is 1 for a character and 0 for a command
static void lcd_send(uint16_t word)
{
    spi_write16_blocking(spi1, &word, 1); // one word from memory, SPI is set to send nine bits
}

// set up SPI and get the LCD ready for text
void lcd_init(void)
{
    spi_init(spi1, LCD_SPI_HZ);
    spi_set_format(spi1, 9, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST); // clock idles low, send the command/character bit first
    gpio_set_function(LCD_CS_GPIO, GPIO_FUNC_SPI); // chip select tells the LCD a transfer is happening
    gpio_set_function(LCD_SCK_GPIO, GPIO_FUNC_SPI); // one clock pulse per bit
    gpio_set_function(LCD_TX_GPIO, GPIO_FUNC_SPI); // send data from the Proton to the LCD

    sleep_ms(50); // wait for the LCD to power up
    lcd_send(0x3c); // set up the two-line display
    sleep_us(40); // give the LCD time to handle this command before the next one
    lcd_send(0x0c); // display on, cursor and blink off
    sleep_us(40);
    lcd_send(0x01); // clear the screen, this one needs more time
    sleep_ms(2);
    lcd_send(0x06); // move the cursor right after each character
    sleep_us(40);
}

// start at the left and write the whole 16-character line
static void lcd_write_line(uint16_t cursor_command, const char *text)
{
    lcd_send(cursor_command); // move the cursor to column 0 of the chosen line
    for(int x = 0; x < LCD_COLUMNS; ++x)
    {
        // pad with spaces so old letters don't stick around
        uint8_t ch = ' ';
        if(*text != '\0') // the zero byte marks the end of the string
        {
            ch = *text;
            text++; // walk to the next letter until we reach that zero byte
        }
        lcd_send(LCD_DATA_BIT | ch); // set bit 8 and leave the character in bits 0-7
    }
}

// update both lines on the LCD
void lcd_show_lines(const char *top, const char *bottom)
{
    lcd_write_line(LCD_LINE_1, top);
    lcd_write_line(LCD_LINE_2, bottom);
}
