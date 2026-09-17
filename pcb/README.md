# HIPI-board - HP-IL Pico Interface — Raspberry Pico 2

*Port of J. Chilla's MicroPython project `JCILD` (HP82163 / HP-41 video
display and HP82161 drive emulator) to C++17, including a port of the
RA8875 driver, plus a touch-screen on-device menu, persistent
configuration, and several HP-IL devices (display, drive, LEDs, PILBox).*

## Bill Of Materials
The HIPI-BOM.pdf shows the BOM for the board.

The main (needed) components are in the beginning of the list.

Note that almost any pin compatible Raspberry Pico 2 card could be used.
It could probably work with a basic Raspberry Pico as well, but all developemtn was done on a Pico 2.

A Pico 2 W could also be used (to enable WiFi and/or Bluetooth), but we have not tried yet.
Also note that the official Pico board uses UCB-Micro, while some other cards uses USB-C if that is preferred.

### 1. Selecting a display
Also a matching display panel is needed.
We have been using the following display panel:

<https://www.buydisplay.com/spi-7-inch-tft-lcd-dislay-module-1024x600-ra8876-optl-touch-screen-panel>

It is a 7" display with SPI interface and capacitive touch (I2C).

In theorie any SPI display could be used (and software adopted to it), and the mounting has to be done in other ways.
Using this display panel, the HIPI-Board is just play-and-play!

When ordering the above display, make sure that:
- Interface:
  - Pin Header Connection-4-Wire SPI
- Power Supply (Typ.)
  - VDD=5.0V
- Touch Panel(Attached by default)
  - 7"Capacitive Touch with Controller
- MicroSD Card Interface
  - Pin Header Connection
- Font Chip
  - n/a - could be selected - nothing we have tried

#### Backlight control - attention needed
There is one change we had to do in the display regarding the backlight.
It could either be controlled by the Pico (you need to manually add a wire from the board)
Or, it could be handled by the panel itself, but then solder jumper J27 and J28 must be reversed.
For best result - remove the solder from J27 and add a solder join on J28 and the panel handles the backlight.

### 2. Mounting the components
The board is made to be easy to mount for anyone with basic soldering skills.
The best way to mount the Pico is to get female headers, and use a Pico with headers, so the processor might be removed or replaced if needed.

### 3. The LED's
The board is prepared to use some LED's, either for status indication, or used by the HPIL controller for fancy effects etc.

There are several options that should be considered before mounting:
- There is a small 3-pin connector that can be used to connect a NeoPixel bar or strip.
  - We have not thought for any longer bars, since there is no external power to the bar
- Or there is place for 1-5 NeoPixels (WS2812) directly on the board
  - Note that they must be mounted from lower to higher number (D12->D16), i.e. if only one is used, it must be in D12.
  - Also note that it is nicer to mount the LED's on the backside of the PCB (for easy viewing when used).
  - If both a bar and NeoPixels LED's are used, the first one on the bar always reflects the ones on the board.
- Up to 5 normal LED's can be mountet at D7-D10, in any order or number.
  - Note that D7 can't be mounted if NeoPixels are goint to be used
  - Note that D11 and D10 can't be used if RX/TX out of the board is goiung to be used.

If no LED's are being used, R11-R15 are not needed, they only are needed for each LED mounted.

### 4. Connectors
The board is made to be easily mounted on a specific display.

JP1 (display and touch interface) and JP2 (SD-card interface) must be mounted.

The suggested jumper JP5 is a screw connector to attach the HP-IL wires, but it is up the you how you want to attach the wires.

Also not that the whole JP1 (all 40 pins) are not needed, I have only mounted two 8 pins (2*4) connectors at each end of JP1, the rest of the pins are unused (in our setup - could be used if a parallell interface is used instead).

Also the connectors J1, J3 and J6 are optional, they could be handy if you want to measure on the board, use RX/TX possibillities, or extend some GPIO's to other external devices.

J4 is also optional, it is a QWIIC connector to allow the card to attach external I2C devices to the board.

### Testing the board
When the board is mounted and the needed connectors (JP1 and JP2) are in place, connect the display and apply power.

If the Pico have the correct firmware, it should start with a Welcome screen.

Verify that touch works - touch the right side of the display to open the configuration and settings menu.

To verify HP-IL, attach HP-IL cables to J5 and shortcut the cable (connect the HP-IL connectors to itself), then run the "Loopback test" under the Config menu, it verifies if the HP-IL interface and cables works as expected.
