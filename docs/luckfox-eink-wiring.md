# Waveshare 2.13" e-Paper HAT — Luckfox Pico Zero Wiring

## 40-pin HAT Connector Compatibility

The Waveshare 2.13" e-Paper HAT has a standard Raspberry Pi 40-pin female header.
The Luckfox Pico Zero also has a 40-pin header described as "GPIO Based on Raspberry Pi
40Pinout" and is advertised as "compatible with some Raspberry Pi HATs."

### Physical compatibility — NOT suitable for direct plug-in

Both PCBs are approximately the same size (HAT: 65×30.2mm, Pico Zero: 65×30mm).
The HAT is designed to stack on top of a larger host board (full-size Pi or Pi Zero).
Plugging the HAT directly onto the Pico Zero would:

- Cover the entire Pico Zero PCB
- Block access to the USB-C port, camera connector, buttons, and Wi-Fi antenna

**Do not use the 40-pin HAT connector. Use the 8-pin cable instead.**

---

## 8-pin Cable Wiring

Connect the HAT to the Luckfox Pico Zero via the HAT's 8-pin FPC/cable connector,
wiring each signal to the appropriate Pico Zero 40-pin header pin.

### Pin mapping

| Signal | HAT 8-pin | Function       | Pico Zero 40-pin | Pico Zero GPIO |
|--------|-----------|----------------|------------------|----------------|
| VCC    | 1         | 3.3V power     | Pin 1 or 17      | 3.3V           |
| GND    | 2         | Ground         | Pin 6, 9, or 14  | GND            |
| DIN    | 3         | SPI MOSI       | Pin 19           | GPIO1_C2       |
| CLK    | 4         | SPI clock      | Pin 23           | GPIO1_C1       |
| CS     | 5         | SPI chip select| Pin 24           | GPIO1_C0       |
| DC     | 6         | Data/command   | Pin 22           | (RPi: GPIO25)  |
| RST    | 7         | Reset          | Pin 11           | (RPi: GPIO17)  |
| BUSY   | 8         | Busy signal    | Pin 18           | (RPi: GPIO24)  |

### Notes on DC / RST / BUSY

The SPI pins (DIN, CLK, CS) land on the Pico Zero's hardware SPI0 peripheral at the
same physical positions as Raspberry Pi SPI0 — electrically compatible.

DC, RST, and BUSY use the same physical pin positions as a Raspberry Pi but the Pico
Zero GPIO numbers differ from Broadcom (BCM) numbers. These are general-purpose
GPIO and do not require a specific hardware peripheral — software remapping is needed
when using Waveshare's sample code:

| Signal | RPi BCM# | Pico Zero physical pin | Pico Zero GPIO |
|--------|----------|------------------------|----------------|
| DC     | GPIO25   | Pin 22                 | Verify via Pico Zero pinout |
| RST    | GPIO17   | Pin 11                 | Verify via Pico Zero pinout |
| BUSY   | GPIO24   | Pin 18                 | Verify via Pico Zero pinout |

Update the GPIO numbers in Waveshare's driver code to match the Pico Zero's GPIO
numbering for pins 11, 18, and 22.

---

## SPI Configuration on Luckfox Pico Zero

The Pico Zero uses the RV1106G3 SoC. Enable SPI0_M0 in the device tree or via the
appropriate kernel configuration. The hardware SPI bus maps to:

| SPI function | Physical pin | RV1106 GPIO |
|-------------|--------------|-------------|
| MOSI        | 19           | GPIO1_C2    |
| SCLK        | 23           | GPIO1_C1    |
| CS0         | 24           | GPIO1_C0    |

---

## References

- [Waveshare 2.13" e-Paper HAT wiki](https://www.waveshare.com/wiki/2.13inch_e-Paper_HAT)
- [Luckfox Pico Zero product page](https://www.waveshare.com/luckfox-pico-zero.htm)
- [Luckfox Pico Zero wiki](https://wiki.luckfox.com/Luckfox-Pico-Zero/Overview/)
