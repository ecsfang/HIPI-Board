# HIPI-board - HP-IL Pico Interface — Raspberry Pico 2

Port of J. Chilla's MicroPython project `JCILD` (HP82163 / HP-41 video
display and HP82161 drive emulator) to C++17, including a port of the
RA8875 driver, plus a touch-screen on-device menu, persistent
configuration, and several HP-IL devices (display, drive, LEDs, PILBox).

## Quick start in VS Code (Raspberry Pi Pico extension)

### 1. Install the extension
Extensions panel → search **"Raspberry Pi Pico"** → install.

### 2. pico-sdk
If you don't already have it via pico-setup:
```bash
git clone -b master https://github.com/raspberrypi/pico-sdk.git ~/pico-sdk
cd ~/pico-sdk && git submodule update --init
```
The extension finds it via `PICO_SDK_PATH`. If your SDK is under
`~/.pico-sdk/sdk/2.2.0/` (default after pico-setup), export:
```bash
export PICO_SDK_PATH=$HOME/.pico-sdk/sdk/2.2.0
```

### 3. Import the project
Do **not** open the folder via `File → Open Folder`. Instead run:

**Command Palette** (`Ctrl+Shift+P`) →
```
Raspberry Pi Pico: Import Project
```

Point to the folder containing `CMakeLists.txt`, select board **pico2**
(Pico 2 / RP2350).

### 4. Build
- **Build** button in the status bar at the bottom, or `Ctrl+Shift+B`
- First build takes ~1–2 min
- The `.uf2` file ends up in `build/hp82163_pico.uf2`

### 5. Flash
Hold **BOOTSEL** on the Pico 2, release after reset, drag-and-drop the
UF2 onto the new drive.

### 6. SD card
Format a micro-SD card FAT32 and place these files at its root:
- `buttons.bmp` — the touch-button strip graphic (drawn right-aligned on boot)
- `logo.bmp` — splash-screen logo (optional; splash falls back to text-only if missing)
- `CONFIG.TXT` — not required; created automatically on first boot with
  default values if missing (see Configuration below)
- Your HP82161 drive image(s) (`.dat` files) — selectable from the on-device
  file picker, or auto-loaded from the last-used filename in `CONFIG.TXT`

---

## Project structure
```
├── CMakeLists.txt                 ← top-level build (three targets: core lib,
│                                     Pico transport lib, pico executable)
├── README.md
├── .vscode/
├── resources/
│   ├── buttons.bmp / buttons.png  ← touch-button strip artwork
│   └── logo.bmp                   ← splash-screen logo
├── include/
│   ├── RA8875.hpp / RA8875Transport.hpp   ← display controller driver + transport interface
│   ├── PicoSpiTransport.hpp / LinuxSpiDevTransport.hpp  ← concrete transports
│   ├── Screen.hpp                 ← HP82163 video-stream emulator (text buffer, scroll-back)
│   ├── hp82163_font.hpp           ← custom 8x16 CGRAM font (ASCII 32-126 + Swedish å/ä/ö)
│   ├── bmp_loader.hpp             ← shared 24-bit BMP loader/drawer (buttons + splash logo)
│   ├── uidialog.hpp               ← on-device touch menu (UiDialog) + splash screen
│   ├── ui_buttons.hpp             ← touch-button hit-testing + press/release bitmap feedback
│   ├── config.hpp                 ← persistent settings (CONFIG.TXT)
│   ├── hpil.h                     ← CDevice: shared HP-IL protocol base class
│   ├── drive.h / tape.h           ← CDrive (HP-IL device) / CTape+CTapeSD+CTapeMem+CTapeFlash (storage backends)
│   ├── display.h                  ← CDisplay: HP-IL device feeding bytes into Screen
│   ├── leds.h / illeds.h          ← LED driver (CLedParser command syntax, see below) + HP-IL LED device
│   ├── pilbox.h                   ← CPilBox HP-IL device
│   ├── touch.h                    ← GSL1680 touch controller interface
│   ├── gslX680fw.h                ← GSL1680 touch controller firmware blob
│   ├── hpil_pio.hpp                ← generated PIO header (from src/hpil.pio)
│   ├── display_test.hpp           ← optional RA8875 diagnostic tests (opt-in, see pico_main.cpp)
│   ├── usb_serial.h / tusb_config.h  ← USB CDC console (cdc0_printf)
│   └── (no-OS-FatFS SD library lives under lib/, vendored, unmodified)
└── src/
    ├── pico_main.cpp               ← entry point: boot sequence, main loop, touch handling
    ├── RA8875.cpp / Screen.cpp
    ├── device.cpp                  ← CDevice implementation
    ├── drive.cpp                   ← CDrive implementation
    ├── display.cpp                 ← CDisplay implementation
    ├── hipi.cpp                    ← wires up all HP-IL devices, trace/debug globals
    ├── hpil.cpp                    ← HP-IL loop frame handling
    ├── hpil.pio                    ← PIO program for the HP-IL physical layer
    ├── leds.cpp / illeds.cpp
    ├── pilbox.cpp
    ├── touch.cpp                   ← GSL1680 driver + touch_get_point()/touch_is_down()
    ├── hw_config.cpp                ← FatFS SD-card hardware config
    ├── my_descriptors.c             ← USB descriptors
    ├── PicoSpiTransport.cpp / LinuxSpiDevTransport.cpp
    └── linux_main.cpp               ← optional Linux demo (HP82163_BUILD_LINUX_EXAMPLE, off by default)
```

No local `pico_sdk_import.cmake` needed — the extension sets up the SDK
via its toolchain file and `CMakeLists.txt` only includes
`${PICO_SDK_PATH}/pico_sdk_init.cmake`.

## Pinout on Pico 2 (same as `share.py`)
| Function      | GPIO     |
|---------------|----------|
| SPI0 SCK      | GP2      |
| SPI0 MOSI     | GP3      |
| SPI0 MISO     | GP0      |
| CS (active L) | GP1      |
| RST (active L)| GP4      |

## Building from the command line
```bash
export PICO_SDK_PATH=$HOME/.pico-sdk/sdk/2.2.0   # or ~/pico-sdk
cmake -G Ninja -B build -S .
cmake --build build
picotool load -f build/hp82163_pico.uf2
```

## Using in your own code
```cpp
#include "PicoSpiTransport.hpp"
#include "RA8875.hpp"
#include "Screen.hpp"

hp82163::PicoSpiTransport t(spi0, 6'000'000, /*cs=*/1, /*rst=*/4);
hp82163::RA8875 display(t, 800, 480);
display.begin();   // configures genuine 16bpp/RGB565 by default (SYSR_16BPP)

hp82163::Screen screen(display, /*color=*/0xFFFF, /*size=*/0, /*brightness=*/255, /*textWidth=*/680);
for (uint8_t b : hp41_stream) screen.pr_char(b);
```
`RA8875::set8Bpp()` still exists but isn't used anywhere in the Pico build
(the display runs in real 16bpp/RGB565 throughout, which also fixed several
subtle color-quantization issues 8bpp had). `set2LayerConfig()` is likewise
unused on Pico; it's only exercised in the optional Linux demo
(`linux_main.cpp`).

---

## On-device touch menu (`UiDialog`)

A five-button touch strip (`Shift`, `OK`, up arrow, down arrow, "back")
drives an on-screen menu, drawn with a rounded yellow frame matching the
button artwork's style. Press **Shift+OK** ("EXIT") from anywhere in the
menu to close it entirely; the "back" button goes up one level at a time.

```
Config
├── Select file     (SD-card .dat picker, with an "Open file?" confirmation)
└── Trace           (Off / On / Extended -- controls bTrace/bExtTrace in hipi.cpp)
Settings
├── Textcolor       (White / Yellow / Green / Cyan / Red)
├── Font size       (0-3, built-in CGRAM scales)
├── Brightness      (20% / 40% / 60% / 80% / 100%)
└── Columns         (Auto / 21 / 28 / 32 / 42 / 85 -- 32 reproduces the
                      original HP82163's column wrap width regardless of
                      font size; the rest are the natural max per font size)
```

While the menu is open, `Screen` output is suspended (incoming HP-41 stream
bytes still update the internal text buffer, just not the visible display)
so the menu can't be drawn over — closing the menu triggers a full redraw
that catches up on anything received while it was open.

Outside the menu (`Screen` closed/idle), the physical arrow buttons scroll
the HP-41 text buffer's scroll-back history instead: up/down move one line
at a time (older/newer), Shift+up/down move a whole page, "back" jumps back
to the live view, and Shift+"back" clears the screen. This is independent
of the HP82163 stream's own `ESC S`/`ESC T` "roll" commands.

Touching a button also gives brief visual feedback: the button's own
bitmap region is redrawn shifted a few pixels (see `kPressDx`/`kPressDy` in
`pico_main.cpp`), then restored on release.

## Configuration

`Config` (`config.hpp`) persists settings as human-readable `key=value`
lines in `CONFIG.TXT` on the SD card:
```
filename=CASSETTE1.DAT
textcolor=65535
trace=0
debug=0
fontsize=0
brightness=255
columns=0
```
Every setter (`setFilename`, `setTextColor`, `setTraceMode`, `setFontSize`,
`setBrightness`, `setColumns`) rewrites the whole file immediately, so it's
always in sync with what's shown in the menu. If `CONFIG.TXT` doesn't exist
yet (or can't be opened for any reason), it's created automatically with
default values on first boot. Unknown keys are ignored when parsing, so
older config files stay loadable as new settings get added.

## Character set

The display uses a custom 8x16 CGRAM font (`hp82163_font.hpp`), not the
RA8875's built-in font — ASCII 32-126, plus Å/Ä/Ö/å/ä/ö at their standard
Latin-1 code points (0xC4/0xC5/0xD6/0xE4/0xE5/0xF6). `RA8875::txtWrite()`
transparently decodes 2-byte UTF-8 sequences in the U+00C0-U+00FF range
into those single-byte codes, so a literal `"Fänge"` in a UTF-8-saved
source file renders correctly — no escaping needed. Note this only applies
to `txtWrite()` (whole C-strings); `txtWriteChar()` takes one raw byte at a
time and can't look ahead, so pass the Latin-1 byte value directly there.

---

## Differences from MicroPython
- Global state in `share.py` → constructor arguments
- `RA8875Transport` = virtual SPI/GPIO interface (easy to swap platform)
- `delayMs()` replaces `time.sleep()`
- `_write_reg` / `_write_reg16` → public `writeReg` / `writeReg16`
- Runs in genuine 16bpp/RGB565 rather than 8bpp/RGB332
- Adds the touch menu, persistent configuration, and Swedish character
  support described above — none of which exist in the original
  MicroPython project

---

## LED control — `CLedParser` syntax

LEDs are controlled by sending compact command strings over CDC/UART,
one character at a time. Commands are grouped as `<leds><command>[<params>]`
and groups are separated by spaces.

### LED selection

| Selector | Meaning |
|----------|---------|
| `1`–`5`  | Individual LEDs (combinable: `"135"` selects 1, 3 and 5) |
| `0`      | All LEDs |

### Commands

| Command | Parameters | Meaning | Example |
|---------|------------|---------|---------|
| `O` | — | Turn on | `12O` |
| `C` | — | Turn off | `345C` |
| `B` | none or `n` | Blink n times (`0` or omitted = infinite) | `15B5` |
| `B` | `n:ms` | Blink n times, `ms` on and off each | `3B10:150` |
| `B` | `n:on:off` | Blink n times, `on` ms on / `off` ms off | `3B10:100:400` |
| `S` | `n` | Set brightness to n % (0–100) | `1S75` |
| `F+` | `t` | Fade on over t ms | `2F+800` |
| `F-` | `t` | Fade off over t ms | `2F-500` |
| `F` | `n:t` | Fade to n % over t ms | `4F30:600` |

### HP-41 Example
This example starts to blink led 1 forever, on 100ms and off 900ms

```
01 LBL 'TEST'
02 3
03 SELECT
04 '1B0:100:900'
05 OUTA
```
> **Note:** `S`, `F+`, `F-`, and `F` require PWM support (`CLED_NO_PWM`
> must not be defined). In non-PWM mode, brightness has no visible effect
> and fade commands snap to on/off immediately.

### Examples

```
"12O"              LEDs 1 and 2 on
"345C"             LEDs 3, 4 and 5 off
"0C"               All LEDs off
"0O"               All LEDs on
"15B5"             LEDs 1 and 5 blink 5 times (200 ms on / 200 ms off)
"3B"               LED 3 blinks forever
"3B10:150"         LED 3 blinks 10 times, 150 ms on / 150 ms off
"3B10:100:400"     LED 3 blinks 10 times, 100 ms on / 400 ms off
"1S75"             LED 1 brightness → 75 %
"0S30"             All LEDs brightness → 30 %
"2F+800"           LED 2 fades on over 800 ms
"2F-500"           LED 2 fades off over 500 ms
"4F30:600"         LED 4 fades to 30 % over 600 ms
"12O 345C"         LEDs 1, 2 on — LEDs 3, 4, 5 off
"0S50 0F+2000"     All LEDs: set brightness to 50 %, then fade on over 2 s
"1B3:100:200 2F+500"  LED 1 blinks 3 times; LED 2 fades on over 500 ms
```

### Notes

- Groups must be separated by a **space** when a numeric parameter is
  immediately followed by a new LED digit, to avoid ambiguity.
  Write `"1B5 23C"` not `"1B523C"` (which would be parsed as
  LED 1 blinking 523 times).
- Sending `\n`, `;`, or calling `flush()` also terminates the last group.
- `B` with no number or `B0` both produce infinite blinking.

---

## References
- MicroPython `share.py` + `RA8875.py`: J. Chilla, March 2026
- HP82163 protocol: HP82163A video display for HP-41
- RA8875: <https://github.com/adafruit/Adafruit_CircuitPython_RA8875>
- pico-sdk: <https://github.com/raspberrypi/pico-sdk>
- Pico VS Code extension: <https://github.com/raspberrypi/pico-vscode>
