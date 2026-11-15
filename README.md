# ECE 362 Audio Visualizer

## Buttons

- Press **21** to switch between **Waveform** and **Spectrum**.
- Press **26** to switch between **RGB** and **Band Mix**. The LCD calls Band Mix `Mix`.

The LCD shows both choices. For example, this means the matrix is drawing the waveform in Band Mix colors.

```text
21 View:Waveform
26 Color:Mix
```

The board saves both choices and loads them the next time it powers on.

## What the matrix shows

- **Waveform** plots the mic signal over time. Quiet input looks like a line near the center. When the sound gets louder, the trace moves farther above and below the center. With RGB colors, the center is blue, the next range is green, and the peaks are red.
- **Spectrum** has 16 bars. Low frequencies are on the left and high frequencies are on the right. A taller bar means a stronger frequency in that range. RGB colors mark bass in red, mids in green, and treble in blue. Try playing a 250 Hz tone and then a 4 kHz tone near the mic to see the tallest bar move right.
- **Band Mix** colors the whole trace or set of bars with a blend based on the sound. Bass adds red, mids add green, and treble adds blue. Strong bass and mids together make yellow.

## How it works

### Mic and DMA

The MAX4466 sends an analog voltage to GP40, which is ADC channel 0. The 12-bit ADC turns that voltage into a number from 0 to 4095. The mic output sits around half its supply voltage when the room is quiet, so silence is around 2048. In [`mic_init()`](src/main.c), the ADC takes 16,000 samples per second and puts them in the FIFO. Each sample triggers a DMA request. DMA reads the same FIFO register each time and writes to the next slot in `audio_samples` until it has 256 readings, which takes 16 ms.

`finish_audio_block()` pauses the ADC once DMA has filled the array, finds the average, draws the next frame, and starts another capture. The waveform uses one of every four samples to fill the 64 columns. Subtracting the average puts quiet input at the middle of the screen. The difference tells us how far up or down to draw each dot.

### FFT and Band Mix

In [`spectrum.c`](src/spectrum.c), `spectrum_measure()` subtracts the average and applies a Hann window. The window fades the ends of the block to reduce the extra frequency content caused by cutting the signal off at the block edges. The FFT keeps splitting the samples into even and odd groups until each group is one sample. It combines the smaller results using sine and cosine weights to account for their timing.

The output has frequency bins spaced `16000 / 256 = 62.5 Hz` apart. A 1 kHz tone is at bin 16. `bin_edges` groups the bins into 16 bars. Inside each group, the code compares `real² + imag²` and takes the square root of the biggest result to get that bar's amplitude.

Band Mix takes the strongest value in each of the three ranges. A range adds its color if it's above the noise floor and at least half as strong as the loudest range. We can turn on multiple LED channels with a bitwise OR.

### RGB matrix and PIO

[`rgb_matrix.c`](src/rgb_matrix.c) reads a 32×64 array of pixels. Each pixel has a bit for red, green, and blue. The HUB75 panel scans two rows at once, so row address 0 lights rows 0 and 16. For each column, the C code packs the three color bits for the upper row and the three for the lower row into six bits.

[`rgb_matrix.pio`](src/rgb_matrix.pio) has two state machines. One shifts those bits into the panel and clocks out 64 columns. The other sets the row address, pulses `LAT` to latch the shifted pixels, and pulls `OE` low for 20 µs to light the rows. It blanks them again before the next pair. The code repeats this for all 16 row pairs. It lights every fourth column on each pass and changes the starting column on the next pass to limit how many LEDs are on at once.

### LCD and EEPROM

[`lcd.c`](src/lcd.c) uses SPI1 and the same 9-bit format as the Lab 6 LCD. Bit 8 tells the LCD whether the word is a command or a character. The code sends a command to move to the start of a line, then writes the characters and pads the line with spaces.

[`settings.c`](src/settings.c) talks to the EEPROM over I2C0 at device address `0x50`. Inside the chip, byte `0` stores the view and byte `2` stores the color. A button press writes the address and one value, waits 5 ms for the write, and reads the byte back. The board applies the change after the readback matches. At startup it reads the saved bytes and uses Waveform and RGB as the defaults for blank or invalid values.
