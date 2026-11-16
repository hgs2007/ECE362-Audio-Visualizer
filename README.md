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

### Frequency bars and Band Mix

The FFT takes 256 readings ordered by time and tells us how strong different frequencies are in that block. [`spectrum.c`](src/spectrum.c) uses two arrays, `fft_real` and `fft_imag`. After the FFT, they hold the cosine and sine parts of each frequency bin.

1. **Get the samples ready.** `spectrum_measure()` subtracts the average and applies a Hann window. The window fades the ends of the block so a sharp cutoff spreads less of a note into nearby bins. The starting samples go into `fft_real`, and `fft_imag` starts at zero. [Hann window formula](https://docs.scipy.org/doc/scipy/reference/generated/scipy.signal.windows.hann.html)
2. **Put the samples in the order the loops expect.** `reverse_bits()` reverses the binary digits of each index. With four samples, `00 01 10 11` becomes `00 10 01 11`, so the samples are arranged `0, 2, 1, 3`. The first pass can work on the even and odd pairs side by side. The code figures out this order once at boot and stores it in `sample_order`. [FFT diagram and bit order](https://www.dspguide.com/CH12.PDF#page=4)
3. **Combine small groups into bigger ones.** `run_fft()` starts with groups of 2, then doubles the group size through 4, 8, and up to 256. For each pair, it uses the sine and cosine table to adjust the phase of the right half before taking the sum and difference. Those become the two results for the pair. This is the operation called a butterfly. There are eight passes of 128 pairs. The shorter transforms get reused across bins, which is why the FFT is a good fit when we need the whole spectrum. [Cooley and Tukey's FFT paper](https://www.cs.jhu.edu/~misha/ReadingSeminar/Papers/Cooley65.pdf)
4. **Turn the bins into bars.** Each bin is `16000 / 256 = 62.5 Hz` apart. A 1 kHz tone is at bin 16. `bin_edges` groups the bins into 16 bars. We compare `real * real + imag * imag` inside each group, then take the square root of the biggest result to get its magnitude. The `4 / 256` scale accounts for the FFT block size, the Hann window, and using only the positive-frequency side.

Band Mix takes the strongest value in each of the three ranges. A range adds its color if it's above the noise floor and at least half as strong as the loudest range. We can turn on multiple LED channels with a bitwise OR.

### RGB matrix and PIO

[`rgb_matrix.c`](src/rgb_matrix.c) reads a 32×64 array of pixels. Each pixel has a bit for red, green, and blue. The HUB75 panel scans two rows at once, so row address 0 lights rows 0 and 16. For each column, the C code packs the three color bits for the upper row and the three for the lower row into six bits.

[`rgb_matrix.pio`](src/rgb_matrix.pio) has two state machines. One shifts those bits into the panel and clocks out 64 columns. The other sets the row address, pulses `LAT` to latch the shifted pixels, and pulls `OE` low for 20 µs to light the rows. It blanks them again before the next pair. The code repeats this for all 16 row pairs. It lights every fourth column on each pass and changes the starting column on the next pass to limit how many LEDs are on at once.

### LCD

[`lcd.c`](src/lcd.c) uses SPI1 with nine bits per transfer. Bit 8 tells the LCD whether the word is a command or a character. The code sends a command to move to the start of a line, then writes the characters and pads the line with spaces.

### EEPROM

[`eeprom.c`](src/eeprom.c) uses I2C0 at 100 kHz, with SDA on GP4 and SCL on GP5. The Pico SDK uses `0x50` to select the chip on the bus. The two address bytes sent after that select a location inside the chip. Byte `0` stores the view, and byte `2` stores the color.

- **Write.** Saving Band Mix sends `00 02 03` to `0x50`. `00 02` is the memory address, and `03` is the value for Band Mix. The write ends with a STOP, which tells the chip to start saving. We check that the SDK reports all three bytes sent, wait 5 ms, then read the byte back. The display changes once the value matches.
- **Read.** To read the color, we write just `00 02` to set the location. The write skips STOP so the read starts with a repeated START. The EEPROM returns the byte at that location. On startup we read bytes `0` through `2` in one go and use bytes `0` and `2` to restore the choices. Blank or invalid values give Waveform and RGB.

To check that saving works, switch to Spectrum and Mix, power the board off and back on, and check that the LCD still shows both choices.
