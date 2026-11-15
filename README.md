# Proton audio visualizer

## First test on the Proton

Open this folder in VS Code with the PlatformIO extension. In the PlatformIO terminal, run the following with the Proton debugger connected. The **Build**, **Upload**, and **Monitor** buttons in PlatformIO do the same thing. [Purdue Proton setup guide](https://github.com/ece362-purdue/proton-labs/blob/main/lab0-intro/README.md)

```text
pio run -e proton
pio run -e proton -t upload
pio device monitor -b 115200
```

1. Look for the red user LED to blink and `mic GP40` readings to repeat in the serial monitor. `MIC_GPIO` in `src/main.c` is set to GP40 to match the MAX4466 output in the wiring handoff. [Purdue ADC lab](https://github.com/ece362-purdue/proton-labs/blob/main/lab4-adc-dma/README.md)
2. Speak close to the mic and compare `peak_to_peak` with the quiet reading. It should rise when you speak. Repeated readings near `min=0` or `max=4095` during loud sounds suggest clipping. The ADC captures 256 samples at 16 kHz per block.

## LCD controls

The SMH1602-J LCD uses SPI1 with CSB on GP13, SCK on GP14, and SDA on GP15. The LCD should be in SPI mode with the SPI pads bridged and the IIC pads open. The code uses the 9-bit format from Lab 6, with bit 8 marking text data. [Lab 6 LCD setup](https://github.com/ece362-purdue/proton-labs/blob/main/lab6-spi/README.md#step-5-use-spi-to-communicate-with-lcdoled-display)

```text
21 View:Waveform
26 Color:Mix
```

The LCD shows both choices all the time. Press **21** to switch between Waveform and Spectrum. Press **26** to switch between RGB and Band Mix. `Mix` on the LCD means Band Mix.

Waveform is the trace over time. Spectrum is the frequency bars. `RGB` uses red for bass, green for mids, and blue for treble. In Waveform, RGB uses blue near the center, green farther out, and red at the peaks.

`Band Mix` uses the strongest FFT bin in each of the three bands. A band lights its RGB channel if it's above the noise floor and at least half as strong as the loudest band. For example, strong bass and mids turn on red and green together to make yellow. The resulting color applies to the current waveform or set of bars.

## Test saving settings

The EEPROM uses I2C0 on GP4 (SDA) and GP5 (SCL) at address `0x50`. Bytes `0` and `2` hold the view and color. The defaults are Waveform and RGB. Earlier Waveform, Spectrum, RGB, and Band Mix values still load. The old GBR and BRG values fall back to RGB.

1. Upload the firmware. Press **21** to select Spectrum and **26** to select Mix. Each press should change just one line on the LCD. The `m` shortcut in Monitor also switches views.
2. Fully power off the Proton and EEPROM, then power them back on. Check that the LCD still shows Spectrum and Mix.

## Show a live waveform

1. Connect the ribbon to the matrix’s **INPUT** connector. The firmware uses GP27–GP32 for `R1 G1 B1 R2 G2 B2`, GP33 for `CLK`, GP34–GP37 for `A B C D`, GP38 for `LAT`, and GP39 for `OE`. Use regulated 5 V panel power and a common ground with the Proton. Adafruit rates the panel for up to 4 A. [Adafruit #2276](https://www.adafruit.com/product/2276) · [Adafruit connector guide](https://learn.adafruit.com/32x16-32x32-rgb-led-matrix/new-wiring)
2. Upload the firmware and press **21** if needed to choose Waveform.
3. In a quiet room, look for a dim line near the center. Play a steady 440 Hz tone and look for the dots to move above and below the center and change color as they reach farther from it.

The ADC fills a 256-sample block using DMA. `src/main.c` subtracts the block mean and converts one of every four samples into a vertical position. `src/hub75.pio` clocks out the pixels and pulses output enable for each of the 16 row pairs. Each scan lights one quarter of the columns to limit simultaneous LEDs. [Pico SDK ADC and DMA example](https://github.com/raspberrypi/pico-examples/tree/master/adc/dma_capture) · [Pico SDK HUB75 example](https://github.com/raspberrypi/pico-examples/tree/master/pio/hub75)

## Show frequency bars

Press **21** if needed to choose Spectrum. With the `RGB` palette, the left four bars are red for bass, the middle eight are green for mids, and the right four are blue for treble. Play pure tones at **250 Hz**, **1 kHz**, and **4 kHz**, one at a time. The tallest bar should move from left to right as the tone gets higher. The bars rise as the sound gets louder. Quiet input can leave the display dark.

Follow one block through the code for checkoff:

1. In `src/main.c`, `mic_init()` starts the 16 kHz ADC and configures DMA to copy 256 samples into memory. `finish_audio_block()` finds the average so both modes can subtract the mic's DC voltage offset.
2. In `src/spectrum.c`, `spectrum_measure()` tapers the ends of the block with a Hann window, then runs the FFT. The FFT splits the data into even and odd samples and combines their frequency results. Each bin is 62.5 Hz apart. `bin_edges` groups the bins into 16 bars, and each bar uses the strongest bin in its range. [Hann window definition](https://docs.scipy.org/doc/scipy/reference/generated/scipy.signal.windows.hann.html)
3. In `src/main.c`, `draw_spectrum()` scales those 16 amplitudes to heights from 0 to 32 pixels. `src/hub75.c` refreshes the same pixel buffer for Waveform and Spectrum.

4. `src/lcd.c` sends the two lines of menu text over SPI. `src/settings.c` writes and verifies one EEPROM byte when a value changes.
