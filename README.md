# ECE 362 Audio Visualizer

The Proton takes sound from a MAX4466 microphone and draws it on a 64 × 32 RGB matrix. The matrix can show the shape of the mic signal or split the sound into frequency bars. The small LCD shows the current view and color settings.

## Buttons

- **21** selects the next setting, going View, Sensitivity, Color, then back to View. The LCD shows the selected setting and its current value.
- **26** changes the selected setting:
  - **View** switches between Waveform and Spectrum.
  - **Sensitivity** steps through Low, Medium, and High, then wraps around.
  - **Color** switches between RGB and Band Mix.

Sensitivity scales how far the waveform and the spectrum bars move for the same sound, at 0.5×, 1×, and 2×. Medium is the default and matches how the display looked before sensitivity was added. This is a software scale on the display. The gain knob on the MAX4466 still sets how loud the signal is at the mic.

## Waveform and mic input

The MAX4466 sends its output to the ADC on GP40. The ADC samples at 16 kHz and puts each reading in a FIFO. DMA copies the readings from the FIFO into an array of 256 samples, so one block is about 16 ms of audio.

The mic voltage sits around half of 3.3 V in a quiet room. After each block, the code subtracts the average to center the waveform. The matrix has 64 columns, so it plots every fourth sample. A louder sound moves the dots farther from the center line. In RGB mode, the dots go from blue near the center to green farther out and red at the peaks. The ADC pauses while the frame is drawn, then starts filling the next block. [`main.c`](src/main.c)

## Spectrum and Band Mix

The spectrum shows 16 bars, with bass on the left and treble on the right. Each bar covers a range of frequencies and gets its height from the strongest frequency in that range. In RGB mode, the bass bars are red, the mids are green, and the treble is blue.

The FFT starts with the same 256 samples used for the waveform. The code subtracts the average and applies a Hann window, which fades the ends of the block so the cutoff spreads less of a tone into nearby bins. It then puts the samples in bit-reversed order so the first pairs are next to each other. The FFT combines pairs with sums and differences, using a sine and cosine table to line up their phases. The groups double in size each pass. [FFT diagrams and bit order](https://www.dspguide.com/CH12.PDF#page=4)

The result is a set of frequency bins spaced `16,000 / 256 = 62.5 Hz` apart. Each bin has a real and an imaginary part. Their combined magnitude gives the strength of that frequency, and `bin_edges` sets which bins go into each bar. [`spectrum.c`](src/spectrum.c)

Band Mix uses the FFT in both views. A band contributes its color if it's above the noise floor and at least half as strong as the loudest band. Bass and mids together turn on red and green, which makes the whole trace or set of bars yellow.

## RGB matrix and PIO

The matrix scans two rows at a time, starting with rows 0 and 16. Each column needs six bits, one for each RGB channel in the upper and lower pixels. The C code packs those bits, and the first PIO state machine clocks them into the panel. The second sets the row address, pulses `LAT` to latch the pixels, and pulls `OE` low to light the rows for 20 µs. The scan lights every fourth column and changes the starting column on each pass to limit how many LEDs are on at once. [`rgb_matrix.c`](src/rgb_matrix.c), [`rgb_matrix.pio`](src/rgb_matrix.pio)

## LCD and saved settings

The LCD uses SPI1. Each transfer is nine bits, with the top bit telling the LCD whether to treat the other eight bits as a command or a character. A command moves the cursor to the start of a line, then the code sends the characters for that line. [`lcd.c`](src/lcd.c)

The EEPROM uses I2C0 on GP4 and GP5. Its bus address is `0x50`. Inside the chip, byte `0` holds the view, byte `1` holds the sensitivity level (`0` Low, `1` Medium, `2` High), and byte `2` holds the color. The selected setting on the LCD is the same number as its byte. Saving Band Mix sends `00 02 03`. The first two bytes select the memory location and `03` is the value stored there. The code waits 5 ms after the write and reads the byte back before applying the choice.

For a read, the code sends just the two address bytes and uses a repeated START to switch to reading. At power-up it reads the saved choices and uses Waveform, Medium, and RGB as defaults. A blank chip reads `0xFF` in byte 1, which isn't a valid level, so it falls back to Medium. [`eeprom.c`](src/eeprom.c)
