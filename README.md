# HIPI Board — HP-IL Pico Interface

HIPI is a small touch-screen box on a Raspberry Pi Pico 2 (RP2350) that sits
on the HP-IL loop of an HP-41, HP-71 or HP-75 and acts as a whole set of
classic HP peripherals at once:

| HIPI acts as…               | Original HP device               | Loop name           |
|-----------------------------|----------------------------------|---------------------|
| Video display               | HP 82163 Video Interface         | `TFDISPLAY`         |
| Cassette drive              | HP 82161A Digital Cassette Drive | `TFDRIVE`           |
| Plotter                     | HP 7470A                         | `TFPLOT`            |
| Temperature/pressure sensor | —                                | `TFTEMP`            |
| LEDs and colour pixels      | —                                | `TFLEDS`, `TFPIXEL` |
| Link to a PC (e.g. pyILPER) | PIL-Box                          | `PILBOX`            |
| Terminal over USB           | —                                | `TFTERM`            |

Cassettes are LIF image files on a micro-SD card. Besides the device views
(display, plotter, tape drive) there is an HP-IL analyzer, a loop map, an
HP-IL signal view, screen savers, screen dumps, and — on the 7" panel — a
mirror of the screen to a PC for recording videos.

The project started as a C++17 port of J. Chilla's MicroPython project
`JCILD` (HP 82163 video display and HP 82161 drive emulator), including its
RA8875 display driver.

## Documentation

| Document | For | Contents |
|----------|-----|----------|
| [User Manual](documents/USER_MANUAL.md) | Users | Getting started, views, menus, devices, settings, troubleshooting |
| [User & Developer Guide](documents/GUIDE.md) | Developers | Setting up, building, flashing, USB interfaces, adding your own HP-IL device |
| [pcb/README.md](pcb/README.md) | Builders | The circuit board and choice of display; [BOM](pcb/HIPI-BOM.pdf), [schematics](pcb/HIPI-Schematics.pdf) |
| [tools/hipiview](tools/hipiview/README.md) | Users | Showing and recording the 7" screen on a PC |
| [tools/ilctrl](tools/ilctrl/README.md) | Users | A command-line HP-IL controller for the PC (PIL-Box CON mode) |

---

## Hardware at a glance

| Part | |
|------|---|
| MCU | Raspberry Pi Pico 2 / Pico 2 W (RP2350) |
| 5" panel | 800 × 480, RA8875, GSL1680 touch — `build/hipi_5_pico.uf2` |
| 7" panel | 1024 × 600, LT7683, FT5316 touch — `build/hipi_7_pico.uf2` |
| Storage | micro-SD card (FAT32 recommended, exFAT works) |
| USB | 4 serial ports (console, PILBOX, terminal, display mirror) and the SD card as a USB drive — see [GUIDE § 7](documents/GUIDE.md#7-usb-interfaces) |

Display SPI wiring (same as the original `share.py`):

| Function   | GPIO |
|------------|------|
| SPI0 SCK   | GP2  |
| SPI0 MOSI  | GP3  |
| SPI0 MISO  | GP0  |
| CS (low)   | GP1  |
| RST        | not wired — the module has its own power-on reset |

---

## Building

Both panel variants are built every time: `build/hipi_5_pico.uf2` (5") and
`build/hipi_7_pico.uf2` (7"). Flash the one that matches your board. The
details, including the PicoLED library, are in
[GUIDE.md § 1–3](documents/GUIDE.md#1-setting-up-the-project).

### In VS Code (Raspberry Pi Pico extension)

1. **Install** the *Raspberry Pi Pico* extension.
2. **pico-sdk 2.2.0:** if the extension hasn't installed it, clone it and
   point `PICO_SDK_PATH` at it:
   ```bash
   git clone -b master https://github.com/raspberrypi/pico-sdk.git ~/pico-sdk
   cd ~/pico-sdk && git submodule update --init
   export PICO_SDK_PATH=$HOME/pico-sdk        # or $HOME/.pico-sdk/sdk/2.2.0
   ```
3. **Import** the project — not *File → Open Folder*, but
   `Ctrl+Shift+P` → **Raspberry Pi Pico: Import Project**, pick the folder
   with `CMakeLists.txt`, and board **pico2** (or **pico2_w**).
4. **Build** with the *Build* button in the status bar or `Ctrl+Shift+B`.
   The first build takes a minute or two.

### From the command line

```bash
export PICO_SDK_PATH=$HOME/.pico-sdk/sdk/2.2.0   # or ~/pico-sdk
cmake -G Ninja -B build -S .
cmake --build build
```

### Flashing

Hold **BOOTSEL** while plugging in (or resetting) the Pico 2 and drop the
`.uf2` onto the drive that appears — or use picotool:

```bash
picotool load -f build/hipi_7_pico.uf2      # or hipi_5_pico.uf2
```

Later updates can be started from the menu: **More → System → Bootsel mode**.

### SD card

Format a micro-SD card (FAT32) and lay it out like this (folder names are in
`include/sd_paths.h`):

```
CONFIG.TXT      settings — created with defaults on first boot
resources/      Tape view pictures, from this repo's resources/ folder:
                hp82161a.bmp, tape-in.bmp, open.bmp, leds.bmp, reels.bmp
lif/            cassette images (*.dat, LIF)
screenshots/    screen dumps — created by the first Screendump
logs/           analyzer logs — created by the first "Log to file"
```

The button strip and the logo are built into the firmware, so HIPI starts
and its menus work without an SD card.

---

## Using HIPI — in brief

The [User Manual](documents/USER_MANUAL.md) has the full story; these are
the essentials.

**Buttons.** Touch the right edge of the screen to slide in the button
strip (it hides again after 5 s):

| Button | Menu closed | In the menu | After **Shift** |
|--------|-------------|-------------|-----------------|
| **OK** | Opens the menu | Chooses | EXIT — closes the menu |
| **▲ / ▼** | Scrolls the text back / forward one line | Moves up / down | One page |
| **X** | Back to the live text | Back one level | CLR — clears the screen |

**Touch.** Top-left corner: info box. Bottom-left corner: the devices on
the loop and their addresses. Swipe left/right: next/previous device view.

**Menus.** OK opens the menu of the view on the screen (e.g. *Clear
plotter* in the Plotter view). Its last row, **More**, leads to HIPI's own
menu:

```
More >
├── Go to view >   Display · Plotter · Tape · Analyzer · Loop map · HP-IL signals
├── Settings >     Screen · Screen saver · Devices · PC link
└── System >       Connect to PC · Loopback test · Scan I2C · Character table ·
                   About · Bootsel mode
```

**While a menu is open** HIPI keeps working: nothing the HP-41 sends is
lost, and the view underneath is up to date when the menu closes.

**Settings** are saved at once to `CONFIG.TXT` on the SD card as
`key=value` lines; unknown keys are ignored, so old files keep working. All
keys are listed in the
[User Manual § 12](documents/USER_MANUAL.md#12-the-settings-file-configtxt).

**Characters.** The display font (`hp82163_font.hpp`) is an 8×16 font with
the HP-41 characters plus Å Ä Ö å ä ö. `txtWrite()` decodes UTF-8, so a
string like `"Fänge"` in the source code shows correctly.

---

## Project structure

```
CMakeLists.txt   build: one target chain per panel (hipi_5_pico, hipi_7_pico)
include/, src/   the firmware (below)
lib/             no-OS-FatFS (SD card), vendored, unmodified
PicoLED/         WS2812 library for TFPIXEL, vendored
resources/       artwork: Tape view pictures (SD card), buttons and logo (built in)
scripts/         build/flash helpers, bmp_to_rgb565.py (turns BMPs into C++)
tools/           PC programs: hipiview (display mirror), ilctrl (HP-IL controller)
documents/       USER_MANUAL.md, GUIDE.md, images
pcb/             hardware: README, BOM, schematics, PCB
```

The firmware, by area (`.h`/`.hpp` in `include/`, `.cpp` in `src/`):

| Area | Files | |
|------|-------|-|
| **Start-up** | `pico_main`, `hipi`, `boot_service`, `hipi_features.h` | Boot sequence and main loop; creates the HP-IL devices; build switches |
| **HP-IL** | `hpil`, `hpil.pio`, `device` | Frames, the PIO physical layer, the `CDevice` base class |
| **HP-IL devices** | `display`, `drive` + `tape`, `plotter`, `illeds`, `ilpixels`, `iltemp`, `terminal`, `pilbox` | `TFDISPLAY`, `TFDRIVE` (LIF on SD), `TFPLOT` (HP-GL), `TFLEDS`, `TFPIXEL`, `TFTEMP`, `TFTERM`, `PILBOX` |
| **Display drivers** | `LT7683`, `RA8875`, `*Transport`, `display_config.h` | 7" and 5" controllers; SPI on the Pico (or spidev on Linux) |
| **Text display** | `Screen`, `hp82163_font.hpp` | HP 82163 emulation: text buffer, escape codes, scroll-back |
| **Views** | `plotterview`, `analyzer`, `loopmap`, `hpil_diag` (+ `.pio`), `chartable` | Display / Plotter / Tape views and switching; analyzer, loop map, HP-IL signals, character table |
| **User interface** | `boardui`, `uidialog.hpp`, `ui_buttons.hpp`, `touch`, `bootscreen` | Button strip, menus, info box, device list, touch, start-up screen |
| **Screen extras** | `backlight`, `clock`, `rtc`, `saver_anim`, `screendump` | Backlight and screen savers, DS3231 clock, screen dumps |
| **Display mirror** | `display_mirror`, `mirror_*.h(pp)` | 7" screen mirrored to a PC (`tools/hipiview`) |
| **Settings & files** | `config.hpp`, `sd_paths.h`, `bmp_loader.hpp`, `lif_info.hpp`, `hw_config` | `CONFIG.TXT`, SD folders, BMP loading, LIF directories, SD card wiring |
| **USB** | `usb_serial.h`, `usb_msc`, `my_descriptors.c`, `tusb_config.h` | Serial ports, the SD card as a USB drive, descriptors |
| **Hardware drivers** | `leds`, `pixels`, `bmp280`, `i2c_device` | LEDs, NeoPixels, pressure sensor, I2C |
| **Fonts & pictures** | `*_font*.cpp`, `glyph_font`, `buttons_image.cpp`, `logo_image.cpp` | Generated data (see `scripts/`) |
| **Other** | `display_test.hpp`, `display_boot_test.hpp`, `linux_main` | Optional diagnostics; a Linux demo (off by default) |

---

## Port notes (vs. the MicroPython original)

- Global state from `share.py` became constructor arguments.
- `RA8875Transport` is a virtual SPI/GPIO interface, so the platform is easy
  to swap (`PicoSpiTransport`, `LinuxSpiDevTransport`).
- `delayMs()` replaces `time.sleep()`; `_write_reg`/`_write_reg16` became
  the public `writeReg`/`writeReg16`.
- The display runs in 16-bit RGB565 instead of 8-bit RGB332.
- New: the touch UI, persistent settings, the extra HP-IL devices and views,
  the LT7683 (7") driver, and the Swedish characters.

---

## Reference: LED commands (`TFLEDS`)

The HP-41 sends compact command strings to `TFLEDS`, for example
`"1B0:100:900"` (LED 1 blinks forever, 100 ms on, 900 ms off). A command is
`<leds><command>[<parameters>]`; several commands are separated by spaces.

**LEDs:** `1`–`5` select single LEDs and can be combined (`135` = LEDs 1, 3
and 5); `0` selects all.

| Command | Parameters | Meaning | Example |
|---------|------------|---------|---------|
| `O`  | —                | On                                          | `12O` |
| `C`  | —                | Off                                         | `345C` |
| `B`  | none, or `n`     | Blink n times, 200 ms on/off (none or `0` = forever) | `15B5` |
| `B`  | `n:ms`           | Blink n times, `ms` on and off              | `3B10:150` |
| `B`  | `n:on:off`       | Blink n times, `on` ms on, `off` ms off     | `3B10:100:400` |
| `S`  | `n`              | Brightness n % (0–100)                      | `1S75` |
| `F+` | `t`              | Fade on over t ms                           | `2F+800` |
| `F-` | `t`              | Fade off over t ms                          | `2F-500` |
| `F`  | `n:t`            | Fade to n % over t ms                       | `4F30:600` |

```
01 LBL "TEST"
02 3
03 SELECT
04 "1B0:100:900"
05 OUTA
```

More examples:

```
"0C"                  all LEDs off
"12O 345C"            LEDs 1 and 2 on, LEDs 3, 4 and 5 off
"0S50 0F+2000"        all LEDs: brightness 50 %, then fade on over 2 s
"1B3:100:200 2F+500"  LED 1 blinks 3 times; LED 2 fades on over 0.5 s
```

- Put a **space** between commands when a number is followed by the next
  LED digit: `"1B5 23C"`, not `"1B523C"` (that is LED 1 blinking 523 times).
- `\n`, `;` or `flush()` also end the last command.
- `S` and the `F` commands need PWM (`CLED_NO_PWM` not defined, see
  `leds.h`); without it, brightness does nothing and fades switch at once.
- Tested on the 5" board.

---

## References

- MicroPython `share.py` + `RA8875.py`: J. Chilla, March 2026
- HP 82163A video interface for the HP-41
- HP-IL interface specification: <https://literature.hpcalc.org/community/hp82166-is-en.pdf>
- *Control the world with HP-IL*: <https://literature.hpcalc.org/community/control-with-hp-il.pdf>
- RA8875 (CircuitPython driver): <https://github.com/adafruit/Adafruit_CircuitPython_RA8875>
- 5" panel (RA8875): <https://www.buydisplay.com/5-inch-tft-lcd-module-800x480-display-controller-i2c-serial-spi>
- 7" panel (LT7683): <https://www.buydisplay.com/spi-7-inch-tft-lcd-dislay-module-1024x600-ra8876-optl-touch-screen-panel>
- pico-sdk: <https://github.com/raspberrypi/pico-sdk>
- Pico VS Code extension: <https://github.com/raspberrypi/pico-vscode>
