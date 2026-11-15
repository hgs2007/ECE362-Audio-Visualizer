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

## Test saving the display mode

The EEPROM uses I2C0 on GP4 (SDA) and GP5 (SCL) at address `0x50`. The firmware stores the mode at EEPROM address `0x0000`. It uses Waveform by default when the stored byte is blank, invalid, or unreadable.

1. Upload the firmware and open Monitor at 115200 baud.
2. Press the onboard button labeled **21** to switch the mode. Press and hold once to check that it saves only once per press. Look for `Saved Spectrum to EEPROM` before the power-cycle test. You can still type `m` in Monitor to switch modes when debugging.
3. Fully power off the Proton and EEPROM, then power them back on. Reopen Monitor and check that the microphone lines end in `mode=Spectrum`.

## Show a live waveform

1. Connect the ribbon to the matrix’s **INPUT** connector. The firmware uses GP27–GP32 for `R1 G1 B1 R2 G2 B2`, GP33 for `CLK`, GP34–GP37 for `A B C D`, GP38 for `LAT`, and GP39 for `OE`. Use regulated 5 V panel power and a common ground with the Proton. Adafruit rates the panel for up to 4 A. [Adafruit #2276](https://www.adafruit.com/product/2276) · [Adafruit connector guide](https://learn.adafruit.com/32x16-32x32-rgb-led-matrix/new-wiring)
2. Upload the firmware, open Monitor, and press button **21** if needed so the reading ends in `mode=Waveform`.
3. In a quiet room, look for a dim green line near the center. Talk near the mic or play a steady 440 Hz tone and look for the dots to move above and below the center.

The ADC fills a 256-sample block using DMA. `src/main.c` subtracts the block mean and converts one of every four samples into a vertical position. `src/hub75.pio` clocks out the pixels and pulses output enable for each of the 16 row pairs. Each scan lights one quarter of the columns to limit simultaneous LEDs. [Pico SDK ADC and DMA example](https://github.com/raspberrypi/pico-examples/tree/master/adc/dma_capture) · [Pico SDK HUB75 example](https://github.com/raspberrypi/pico-examples/tree/master/pio/hub75)

## Next milestones

1. **Spectrum.** Apply a Hann window and a 256-point FFT to each microphone block. Group frequency bins into bars and test with a known tone. [Purdue ADC and DMA lab](https://github.com/ece362-purdue/proton-labs/blob/main/lab4-adc-dma/README.md)
2. **SPI LCD.** Use the SMH1602-J 16×2 character LCD to display the mode and sensitivity. Reuse the lab SPI setup and start by displaying `Waveform`. [Purdue lab kit display references](https://ece362-purdue.github.io/proton-labs/lab-kit/)
3. **Settings.** Extend the EEPROM code to save sensitivity and color when the user changes them. Power-cycle and check that both settings are restored.
