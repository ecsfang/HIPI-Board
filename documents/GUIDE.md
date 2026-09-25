# HIPI Board — User & Developer Guide

This guide covers everything from a fresh checkout to adding your own
HP-IL devices:

1. [Setting up the project](#1-setting-up-the-project)
2. [Building the firmware](#2-building-the-firmware)
3. [Flashing the Pico](#3-flashing-the-pico)
4. [Preparing the SD card](#4-preparing-the-sd-card)
5. [Using the board](#5-using-the-board)
6. [The on-screen menus](#6-the-on-screen-menus)
7. [USB interfaces](#7-usb-interfaces)
8. [Built-in HP-IL devices](#8-built-in-hp-il-devices)
9. [Adding your own HP-IL device](#9-adding-your-own-hp-il-device)
10. [Troubleshooting](#10-troubleshooting)

For hardware (schematics, BOM, choice of display) see `pcb/README.md`.
For the LED command syntax and the RA8875 driver API see `README.md`.

---

## 1. Setting up the project

### 1.1 Prerequisites (Ubuntu)

```bash
sudo apt update
sudo apt install git cmake ninja-build python3 build-essential
```

The ARM cross-compiler, Pico SDK and `picotool` are installed automatically
by the **Raspberry Pi Pico** VS Code extension (recommended). If you want to
build purely from the command line without the extension, also install:

```bash
sudo apt install gcc-arm-none-eabi libnewlib-arm-none-eabi libstdc++-arm-none-eabi-newlib
```

The project is built and tested with **Pico SDK 2.2.0** and toolchain
**14_2_Rel1** (pinned at the top of `CMakeLists.txt`).

### 1.2 Clone the repository

The project uses two git submodules (the FatFS SD-card library and PicoLED),
so clone with `--recurse-submodules`:

```bash
mkdir -p ~/Projects && cd ~/Projects
git clone --recurse-submodules https://github.com/<owner>/HIPI-Board.git
cd HIPI-Board
```

> Replace `<owner>` with the actual GitHub account. The helper scripts in
> `scripts/` assume the project lives in `~/Projects/HIPI-Board`.

If you already cloned without submodules:

```bash
git submodule update --init --recursive
```

After this you should have:

```
HIPI-Board/
├── lib/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico/   ← submodule
├── PicoLed/                                ← submodule
├── include/  src/  resources/  scripts/  pcb/
└── CMakeLists.txt
```

### 1.3 Open in VS Code

1. Install the **Raspberry Pi Pico** extension (Extensions panel → search
   "Raspberry Pi Pico").
2. Do **not** use *File → Open Folder* the first time. Instead:
   `Ctrl+Shift+P` → **Raspberry Pi Pico: Import Project**.
3. Select the folder containing `CMakeLists.txt`, board **pico2**, SDK
   **2.2.0**.

The extension downloads the SDK/toolchain into `~/.pico-sdk/` if needed and
configures CMake for you.

---

## 2. Building the firmware

### 2.1 Panel variants

The same source builds for two display panels:

| Panel | Controller | Touch   | Output file              |
|-------|------------|---------|--------------------------|
| 5"    | RA8875     | GSL1680 | `build/hipi_5_pico.uf2`  |
| 7"    | LT7683     | FT5316  | `build/hipi_7_pico.uf2`  |

By default **both** are built. Flash the one matching your board.

### 2.2 From VS Code

Press the **Compile** button in the status bar (or `Ctrl+Shift+B`). The
first build takes 1–2 minutes.

### 2.3 From the command line

```bash
export PICO_SDK_PATH=$HOME/.pico-sdk/sdk/2.2.0   # or wherever your SDK is
cmake -G Ninja -B build -S .
cmake --build build
```

To build only one panel (faster when iterating):

```bash
cmake -DHIPI_BUILD_PANELS=7 -B build      # 7" only
cmake -DHIPI_BUILD_PANELS=5 -B build      # 5" only
cmake -DHIPI_BUILD_PANELS="5;7" -B build  # both (default)
```

If you change the *structure* of `CMakeLists.txt` (new targets, new
libraries), delete `build/` and configure again.

### 2.4 Helper scripts

| Script                  | Purpose |
|-------------------------|---------|
| `scripts/make_hipi.sh`  | Clean build of one panel |
| `scripts/do_all.sh`     | Build, wait for the Pico in BOOTSEL, then flash. Arguments: `[zip-file] [5\|7] [ALL]` (`ALL` = clean rebuild) |
| `scripts/flash_hipi.sh` | Flash an already-built UF2 |
| `scripts/hpil_parser.py`| Decode an HP-IL trace log into readable text |

The scripts contain hard-coded paths (`~/Projects/HIPI-Board`,
`/media/thomas/RP2350`) — adjust `PROJECT_DIR` and `PICO_MOUNT` to your
system before use.

---

## 3. Flashing the Pico

The Pico 2 (RP2350) is flashed by copying a `.uf2` file to it while it is
in **BOOTSEL** mode, where it shows up as a USB drive named `RP2350`.

### 3.1 Enter BOOTSEL mode

Choose whichever is convenient:

- **Button:** Hold the **BOOTSEL** button on the Pico, connect USB (or press
  reset), then release.
- **Menu:** On a running board, open **Settings → Bootsel mode**. The board
  reboots straight into BOOTSEL — no need to reach the button inside a case.

### 3.2 Copy the firmware

- **Drag and drop:** copy `build/hipi_7_pico.uf2` (or `hipi_5_pico.uf2`)
  onto the `RP2350` drive. The Pico reboots automatically when the copy is
  complete.
- **Command line:**
  ```bash
  cp build/hipi_7_pico.uf2 /media/$USER/RP2350/ && sync
  ```
- **picotool** (board already in BOOTSEL):
  ```bash
  picotool load -x build/hipi_7_pico.uf2
  ```
- **Script:** `scripts/do_all.sh 7` builds, waits until the drive is really
  mounted and writable, and copies the file.

### 3.3 Verify

After reboot you should see the splash screen with the version number,
followed by a short boot summary (USB status, current drive file, trace
mode, font, columns) and the device list.

---

## 4. Preparing the SD card

Format a micro-SD card as **FAT32** or **exFAT** and copy these files to
the root. Both work; cards larger than 32 GB are normally delivered as exFAT.
To reformat on Ubuntu (this erases the card):

```bash
sudo apt install exfatprogs
lsblk                              # find the card, e.g. /dev/sdb1
sudo mkfs.exfat -n HIPI /dev/sdb1  # or: sudo mkfs.vfat -F 32 -n HIPI /dev/sdb1
```

Files to copy:

| File            | Required | Purpose |
|-----------------|----------|---------|
| `buttons.bmp`   | **Yes**  | Artwork for the touch-button strip (from `resources/`). Without it there are no buttons and no menu. |
| `logo.bmp`      | No       | Splash-screen logo (from `resources/`). Text-only splash if missing. |
| `hp82161a.bmp`  | No       | 7" panel only: picture for the **Tape** view (from `resources/`). The view is left out if missing. |
| `tape-in.bmp`   | No       | 7" panel only: the cassette shown in the Tape view's lid window when a file is selected (from `resources/`). |
| `open.bmp`      | No       | 7" panel only: the Tape view's lid area with the lid open, shown while the file picker is open (from `resources/`). |
| `leds.bmp`      | No       | 7" panel only: the lit POWER and BUSY LEDs and the power switch in its ON position, for the Tape view (from `resources/`). |
| `*.dat`         | No       | HP82161 cassette/drive images, selectable from the menu. |
| `CONFIG.TXT`    | No       | Created automatically with defaults on first boot. |

Once the board is running you can also copy files to the SD card over USB —
see **Config → Connect to PC** below.

### 4.1 CONFIG.TXT

All settings are stored as plain `key=value` lines and are written
immediately whenever you change something in the menu:

```
filename=CASSETTE1.DAT
textcolor=65535
trace=0
debug=0
fontsize=0
brightness=255
columns=0
disabled_devices=TFPIXEL,TFTERM
```

`disabled_devices` lists devices switched off in the **Devices** menu.
Unknown keys are ignored, so older files keep working.

---

## 5. Using the board

### 5.1 The button strip

The right-hand button strip is hidden during normal use so the text area
gets the full width. **Touch anywhere in the right-hand edge area** to show
it. That first touch only reveals the strip — it does not press anything.
The strip hides again after 5 seconds of inactivity (it stays visible while
a menu is open).

```
 ┌──────┐
 │ ████ │  Shift   (yellow) — modifies the NEXT button press
 │ EXIT │
 │  OK  │  OK      — open menu / select        Shift+OK = EXIT
 │  ◄   │
 │  ▲   │  Up                                   Shift+▲  = page up
 │  ►   │
 │  ▼   │  Down                                 Shift+▼  = page down
 │ CLR  │
 │  X   │  X / back                             Shift+X  = CLR
 └──────┘
```

**Shift** is a latch: tap Shift, then tap the other button. The yellow
labels on the artwork show the shifted functions.

### 5.2 With the menu closed

| Button    | Action |
|-----------|--------|
| OK        | Open the main menu |
| ▲ / ▼     | Scroll the text history one line (older / newer) |
| Shift+▲/▼ | Scroll one page |
| X         | Jump back to the live view |
| Shift+X   | Clear the screen |
| Shift+OK  | Hide the button strip |

The scroll-back history is independent of the HP82163 `ESC S`/`ESC T` roll
commands sent by the HP-41.

### 5.3 Touch gestures and corners

| Gesture                     | Action |
|-----------------------------|--------|
| Tap **top-left corner**     | Info box with the current settings (auto-hides after 5 s, or tap to close) |
| Tap **bottom-left corner**  | Live list of HP-IL devices: address, name, accessory ID (SAI), enabled state |
| Vertical swipe              | Scroll the device list (when it is open) |
| Horizontal swipe            | Cycle the view: **Display** (text) → **Plotter** → **Tape** (7" only), and back in the other direction |

The device list performs a live AAD/SAI/SDI enumeration of the internal
devices. Avoid opening it while a controller is in the middle of a transfer.

### 5.4 Status LEDs

The board's LEDs show HP-IL activity (frame being processed) and idle
polling. The USB and PILBOX indicators in the button strip light up when
USB is enumerated and when a PC application has put the PILBox into an
active mode.

---

## 6. The on-screen menus

Open with **OK**. Navigate with **▲/▼**, select with **OK**, go up one level
with **X**. **Shift+OK** closes the menu from any level.

While the menu is open, incoming HP-IL display data is still received, so
nothing is lost.

```
Main menu
├── Config
│   ├── Select file        .dat picker on the SD card, with "Open file?" confirmation
│   ├── Trace              Off / On / Extended
│   ├── Connect to PC      Expose the SD card as a USB drive (toggles to "Disconnect from PC")
│   ├── Loopback test      Physical HP-IL loop test (needs a loop cable)
│   └── Scan I2C           Scan the I2C bus and show found addresses
├── Settings
│   ├── Textcolor          White / Yellow / Green / Cyan / Red
│   ├── Font size          0 – 3
│   ├── Brightness         20 % / 40 % / 60 % / 80 % / 100 %  (live preview)
│   ├── Columns            Auto / 25 / 32 / 33 / 50 / 100
│   └── Bootsel mode       Reboot into BOOTSEL for firmware update
├── Devices                Enable/disable each HP-IL device (OK toggles [ON]/[OFF])
└── Display
    ├── Display            Show the text display
    ├── Plotter            Show the plotter canvas
    ├── Tape               Show the HP82161A tape drive (7" panel only)
    ├── Clear plotter      Erase the plotter drawing
    └── Clear screen       Erase the text display
```

### 6.1 Config

- **Select file** — lists all `.dat` files in the SD root, with **No media**
  as the first row. The current file is pre-selected. Choosing **No media**
  deselects the file: the drive reports that no tape is inserted, the Tape
  view shows the drive without a cassette, and the info box and boot
  summary show "No media". After confirmation the drive (`TFDRIVE`) switches to the
  new image and the name is saved to `CONFIG.TXT`.
- **Trace** — controls logging on the USB debug port:
  - *Off* — no frame log.
  - *On* — one line per non-routine HP-IL frame with mnemonics.
  - *Extended* — additionally dumps the state of every device for every
    frame. Useful for debugging a new device, very verbose.
- **Connect to PC** — the SD card appears as a USB mass-storage drive on
  the PC, so you can copy `.dat` files without removing the card. While
  connected, drive access from HP-IL is paused and `CONFIG.TXT` is not
  written. Always choose **Disconnect from PC** (and eject on the PC side)
  before going back to normal use.
- **Loopback test** — connect the HP-IL OUT connector directly to IN with a
  cable, then press OK. Every frame value is sent and must come back
  unchanged. A progress bar is shown; the result is either
  *Loopback OK!* or the first failing frame. Touch anywhere to continue.
- **Scan I2C** — scans addresses 0x08–0x77 on the shared I2C bus (the one
  the touch controller and BMP280 sit on) and shows a grid of responding
  addresses. Handy when adding a new I2C device.

### 6.2 Settings

- **Textcolor / Font size / Brightness** — apply immediately and are saved.
  Brightness previews as you move; **X** restores the previous level.
- **Columns** — forces the line-wrap width. *Auto* uses the natural width
  for the current font size; *32* reproduces the original HP82163 wrap
  width.
- **Bootsel mode** — see [section 3.1](#31-enter-bootsel-mode).

### 6.3 Devices

Lists every internal HP-IL device with `[ON]` or `[OFF]`. A disabled
device is completely removed from the loop: it processes no frames and does
not take an address during auto-addressing — exactly as if it were
unplugged. The setting is saved per device name.

### 6.4 Display

Chooses which output fills the screen: the HP82163-style text display, the
HP7470A-style plotter canvas (`TFPLOT`), or (7" panel only) the Tape view.
Display and Plotter keep receiving data in the background regardless of
which is shown.

**Tape view.** Shows a picture of an HP82161A cassette drive. It is loaded
from `hp82161a.bmp` once at boot into a spare display layer, so switching to it
is instant. Tap the **cassette window** or the **OPEN** button to go
straight to the `.dat` file picker. Choosing a file, or pressing **X**,
returns to the Tape view. When a `.dat` file is selected, a cassette with
the file name on its label is seen through the lid window; while the file
picker is open (from the Tape view or from **Config**) the lid is shown
open with the drive empty. The **POWER** LED is lit while `TFDRIVE` is
enabled (**Devices** menu), and **BUSY** lights up while the drive reads,
writes or formats its file -- not for ordinary HP-IL traffic. The power
switch shows ON while `TFDRIVE` is enabled; tapping it enables/disables
the drive, exactly like the **Devices** menu (and saved the same way). The view is not available on the 5" panel, which has no spare
display memory.

---

## 7. USB interfaces

With USB connected, the board enumerates as a composite device:

| Interface | Linux device      | Purpose |
|-----------|-------------------|---------|
| CDC 0     | `/dev/ttyACM0`    | Debug console / trace log |
| CDC 1     | `/dev/ttyACM1`    | PILBox link to a PC emulator (e.g. pyILPER) |
| CDC 2     | `/dev/ttyACM2`    | `TFTERM` terminal bridge (e.g. HP-71B `DISPLAY IS`/`KEYBOARD IS`) |
| MSC       | USB drive         | SD card, only while **Connect to PC** is active |

Device numbering may differ if other CDC devices are connected. To read the
debug log:

```bash
minicom -D /dev/ttyACM0     # or: screen /dev/ttyACM0
```

If your user cannot open the ports, add yourself to the `dialout` group
(`sudo usermod -aG dialout $USER`, then log in again).

---

## 8. Built-in HP-IL devices

The devices are chained in this order, which is also their order in the
HP-IL loop (see `hipi_init()` in `src/hipi.cpp`):

| Name        | Class        | SAI  | Function |
|-------------|--------------|------|----------|
| `TFDISPLAY` | `CDisplay`   | 0x3E | HP82163 video display emulation |
| `TFDRIVE`   | `CDrive`     | 0x10 | HP82161 cassette drive, backed by a `.dat` on SD |
| `TFLEDS`    | `CHipiLed`   | 0xEE | LED control (syntax in `README.md`) |
| `TFPIXEL`   | `CHipiPixel` | 0xED | Pixel/LED strip control |
| `TFTEMP`    | `CHipiTemp`  | 0x51 | BMP280 temperature/pressure sensor over I2C |
| `PILBOX`    | `CPilBox`    | —    | Forwards the loop to a PC over CDC 1 |
| `TFPLOT`    | `CPlotter`   | 0x60 | HP7470A-compatible plotter (HP-GL subset) |
| `TFTERM`    | `CTerminal`  | 0x4E | Terminal bridge over CDC 2 |

Devices *after* `PILBOX` in the chain come after any virtual devices running
on the PC.

Example — reading the temperature from an HP-41 (use the address shown in
the bottom-left device list, here 5):

```
01 LBL "TEMP"
02 5
03 SELECT
04 "MT"
05 OUTA
06 INA
07 AVIEW
08 END
```

`"MT"` selects temperature (`"MP"` selects pressure in hPa). Each `INA`
triggers a new reading, returned as text, e.g. `23.5`.

---

## 9. Adding your own HP-IL device

### 9.1 How the loop works in firmware

Every frame received from the physical loop is passed through each enabled
device in turn (`hipi_loop()` in `src/hipi.cpp`):

```cpp
for (CDevice* dev : devices) {
    if (dev->enabled()) {
        rx_frame = dev->hpil(rx_frame);   // each device may replace the frame
    }
}
loop.sendFrame(rx_frame);                 // result goes on to the next real device
```

`CDevice::hpil()` (`src/device.cpp`) already handles the generic protocol:
IFC, DCL/SDC, UNL/UNT, LAD/TAD addressing, AAD/AAU auto-addressing, SAI and
SDI. Your device only implements what is specific to it:

| Method                         | Required | Called when |
|--------------------------------|----------|-------------|
| `clear()`                      | **Yes**  | DCL, or SDC while we are listener |
| `doListener(cmd, rtn)`         | Usually  | A frame arrives while we are addressed as listener |
| `doTalker(cmd, rtn)`           | Usually  | A frame arrives while we are addressed as talker (SDA, SST, NRD, echoed data...) |
| `idle()`                       | No       | Every main-loop pass without a frame — for polling/background work |
| `ifc()`                        | No       | Interface Clear |
| `preProc(cmd)`                 | No       | Before generic handling of every frame |
| `show()`                       | No       | Extended trace — print your internal state |

Keep `hpil()`, `doListener()` and `doTalker()` fast. They run in the frame
path, and anything slow (long I2C transactions, drawing, SD writes) delays
the whole loop. Do slow work in `idle()` or in a separate view module (as
the plotter does).

### 9.2 The constructor

```cpp
CDevice(const char* name, IL_ADDR_t sai, IL_ADDR_t aau, IL_Type_e type);
```

- **name** — max 15 characters. Used as the SDI response (what `FINDID`
  and the device list see), in the Devices menu, and as the key in
  `disabled_devices`. Must be unique.
- **sai** — Accessory ID, returned for SAI. Pick one from the HP-IL
  specification (Appendix C.4, "Classes and Types Defined"), e.g. `0x5x`
  instrumentation, `0x6x` graphics/plotters.
- **aau** — address after Auto Address Unconfigure; use 31 (unaddressed)
  unless you have a reason not to.
- **type** — tag from `IL_Type_e` in `include/hpil.h`. Add a new value if
  other code needs to recognise your device.

### 9.3 Step by step

1. **Create the files** `include/ilmydev.h` and `src/ilmydev.cpp` with a
   class derived from `CDevice` (skeleton below).
2. **Add the source file** to `add_executable(${EXE_TARGET} ...)` in
   `CMakeLists.txt`:
   ```cmake
       src/ilmydev.cpp
   ```
   If you need another SDK library (e.g. `hardware_adc`), add it to
   `target_link_libraries(${EXE_TARGET} ...)`.
3. **Register the device** in `hipi_init()` in `src/hipi.cpp`:
   ```cpp
   #include "ilmydev.h"
   ...
   devices.push_back(new CMyDevice("TFMYDEV", 0x52));
   ```
   The position in `devices` is the position in the loop. Put local
   devices **before** `PILBOX` unless they need to come after the PC.
   `devices.back()->last(true)` must stay at the end, after all
   `push_back` calls.
4. **Build, flash, and check** the bottom-left device list: your device
   should appear with an address and your SAI. The Devices menu,
   enable/disable persistence and the boot-time device listing all work
   automatically.
5. **Debug** with **Config → Trace → Extended** and the log on
   `/dev/ttyACM0`. Implement `show()` to print your own state.

### 9.4 Skeleton — a simple talker/listener

The standard pattern (used by `CHipiTemp` and `CPlotter`): the listener
collects command bytes, the talker answers SDA by sending a queued string
one byte per frame and finishes with ETO.

```cpp
// include/ilmydev.h
#pragma once
#include <vector>
#include <cstdint>
#include "hpil.h"

class CMyDevice : public CDevice {
public:
    CMyDevice(const char* name, IL_ADDR_t sai, IL_ADDR_t aau = 31)
        : CDevice(name, sai, aau, NONE) {}

    void clear() override;
    void ifc() override;
    void doListener(IL_CMD_t cmd, IL_CMD_t* rtn) override;
    void doTalker(IL_CMD_t cmd, IL_CMD_t* rtn) override;

private:
    void prepareReply();              // fills outQueue_ with the answer

    std::vector<std::uint8_t> outQueue_;
    bool midTransfer_ = false;
};
```

```cpp
// src/ilmydev.cpp
#define MODULE "MYDEV"
#include "ilmydev.h"
#include "usb_serial.h"
#include <string>

void CMyDevice::clear() {
    outQueue_.clear();
    midTransfer_ = false;
}

void CMyDevice::ifc() {
    setIdle();
}

void CMyDevice::doListener(IL_CMD_t cmd, IL_CMD_t* rtn) {
    *rtn = cmd;                       // always pass the frame on unchanged
    if (!IS_DATA(cmd)) return;        // only data bytes are commands for us
    const std::uint8_t c = cmd & 0xFF;
    MTRC_LOGF("received 0x%02X", c);
    // ... interpret the byte (single-letter commands, parameters, etc.)
}

void CMyDevice::doTalker(IL_CMD_t cmd, IL_CMD_t* rtn) {
    if (cmd == NRD) return;           // Not Ready for Data -- controller stops

    // Answer SDA, and the echo of our own byte while a transfer is running
    if (cmd != SDA && !(midTransfer_ && IS_DATA(cmd))) return;

    if (cmd == SDA && !midTransfer_) prepareReply();

    if (outQueue_.empty()) {
        *rtn = ETO;                   // End of Transmission OK
        midTransfer_ = false;
    } else {
        *rtn = outQueue_.front();     // next byte replaces the frame
        outQueue_.erase(outQueue_.begin());
        midTransfer_ = true;
    }
}

void CMyDevice::prepareReply() {
    const std::string s = "HELLO\r\n";
    outQueue_.assign(s.begin(), s.end());
}
```

Logging macros from `usb_serial.h`: `LOGF` (always), `MTRC_LOGF` (only with
Trace on), `MDBG_LOGF` (only with Extended trace). The `M` variants prefix
the line with `MODULE`.

> Some devices (e.g. `CTerminal`) need a fresh SDA per byte instead of the
> continuous drain above. See the comment at the top of `include/terminal.h`
> if your controller behaves that way.

### 9.5 Example: an I2C device (like `TFTEMP`)

I2C devices are split in two layers:

1. **Chip driver** derived from `CI2CDevice` (`include/i2c_device.h`) —
   knows only the chip: registers, conversion, calibration. Example:
   `CBmp280` in `include/bmp280.h` / `src/bmp280.cpp`.
2. **HP-IL device** derived from `CDevice` — knows only the HP-IL protocol
   and holds a reference to the chip driver. Example: `CHipiTemp` in
   `include/iltemp.h` / `src/iltemp.cpp`.

This keeps the chip driver reusable and testable without HP-IL.

**The shared bus.** All I2C devices share `i2c0` on GP16 (SDA) / GP17 (SCL)
with the touch controller. `touchInit()` initialises the bus and
`pico_main.cpp` then calls `CI2CBus::markInitialized(touch_i2c)`, so your
driver's `initBus()` will not re-initialise it. Use **Config → Scan I2C** to
confirm the chip's address before writing any code.

Chip driver skeleton (a BH1750 light sensor — an example only, not part of the project):

```cpp
// include/bh1750.h
#pragma once
#include "i2c_device.h"

class CBh1750 : public CI2CDevice {
public:
    explicit CBh1750(i2c_inst_t* bus, std::uint8_t addr = 0x23)
        : CI2CDevice(bus, addr) {}

    bool begin(uint sda, uint scl) {
        initBus(sda, scl);                           // no-op if the bus is ready
        const std::uint8_t powerOn = 0x01, contHiRes = 0x10;
        return i2c_write_blocking(bus_, address_, &powerOn, 1, false) == 1 &&
               i2c_write_blocking(bus_, address_, &contHiRes, 1, false) == 1;
    }

    // Returns lux * 10 (one decimal) -- false if the chip does not answer
    bool readLuxTenths(std::int32_t& out) {
        std::uint8_t b[2];
        if (i2c_read_blocking(bus_, address_, b, 2, false) != 2) return false;
        const std::uint32_t raw = (static_cast<std::uint32_t>(b[0]) << 8) | b[1];
        out = static_cast<std::int32_t>(raw * 10 / 12);   // lux = raw / 1.2
        return true;
    }
};
```

For chips with a normal register map, use the protected helpers
`writeReg8()`, `readReg()` and `readReg8()` instead of raw
`i2c_*_blocking()` calls.

HP-IL wrapper: copy the skeleton from 9.4 and let `prepareReply()` call the
chip driver:

```cpp
class CHipiLight : public CDevice {
public:
    CHipiLight(const char* name, IL_ADDR_t sai, CBh1750& sensor, IL_ADDR_t aau = 31)
        : CDevice(name, sai, aau, NONE), sensor_(sensor) {}
    // clear(), ifc(), doListener(), doTalker() as in 9.4
private:
    void prepareReply();   // sensor_.readLuxTenths() -> "123.4\r\n" or "ERR\r\n"
    CBh1750& sensor_;
    std::vector<std::uint8_t> outQueue_;
    bool midTransfer_ = false;
};
```

Registration in `hipi_init()`, following the `TFTEMP` pattern:

```cpp
static CBh1750* lightSensor = nullptr;
...
lightSensor = new CBh1750(touch_i2c);
if (!lightSensor->begin(TOUCH_SDA, TOUCH_SCL)) {
    LOGF("\r\n * WARNING: BH1750 not responding -- TFLIGHT will report ERR");
}
devices.push_back(new CHipiLight("TFLIGHT", 0x51, *lightSensor));
```

Tips for I2C devices:

- A single transaction must be short. A 100 kHz read of a few bytes is
  fine inside `doTalker()`; anything with a long conversion time should be
  started earlier (e.g. by a listener command) and polled in `idle()`.
- Always return something on SDA — `"ERR"` if the chip does not answer —
  so the controller never waits forever.
- Because the bus is shared with touch, a hanging I2C device also blocks
  touch. Use the `*_timeout_us()` variants of the SDK calls if the chip may
  be missing.

### 9.6 Example: a device with graphical output (like `TFPLOT`)

`CPlotter` shows how to split a device that draws on the screen:

- **`CPlotter`** (`src/plotter.cpp`) — pure HP-IL device and HP-GL
  interpreter. It keeps plotter state and a list of line segments, and
  reports what happens through callbacks (`setDrawCallback()`,
  `setClearCallback()`, `setMoveCallback()`, `setPenChangedCallback()`).
  It never touches the display.
- **`plotterview`** (`src/plotterview.cpp`) — owns the display while the
  plotter view is active. `plotterview_init()` registers the callbacks,
  scales plotter coordinates to screen coordinates, and redraws everything
  from `segments()` when the view is switched in.

To add your own graphical device:

1. Write the device in the same style: state + callbacks, no drawing code.
2. Write a view module (e.g. `src/myview.cpp`) that registers the
   callbacks and draws through `DisplayDriver`.
3. Add a value to `DisplayOutput` in `include/plotterview.h` (and update
   `kDisplayOutputCount`; extend `plotterview_isAvailable()` if the view
   isn't available on every board), and handle it in `plotterview_setOutput()` /
   `plotterview_cycleOutput()`, so that swipe and the **Display** menu can
   select it. Add a menu label in
   `kDisplayMenuLabels` in `include/uidialog.hpp`.
4. Call the view's `init()` from `pico_main.cpp`, after `hipi_init()`
   (the device must exist) — the same place `plotterview_init()` is called.

Store what you draw (as `CPlotter` does with `segments_`) so the view can be
redrawn completely when the user switches back to it.

### 9.7 Checklist

- [ ] Class derived from `CDevice`, `clear()` implemented
- [ ] Unique name (≤ 15 characters) and a sensible SAI
- [ ] `.cpp` added to `CMakeLists.txt`
- [ ] `push_back` in `hipi_init()` at the right position, before `last(true)`
- [ ] Frame handlers are fast; slow work is in `idle()` or a view module
- [ ] Talker always ends with ETO; listener always passes the frame on
- [ ] Code comments in English

---

## 10. Troubleshooting

| Symptom | Check |
|---------|-------|
| No buttons on the screen | `buttons.bmp` missing from the SD root, or the SD card is not FAT32/exFAT |
| "PANIC: f_mount error" in the log | SD card not inserted / not formatted / bad contact |
| HP-IL doesn't respond | Run **Config → Loopback test** with a cable from OUT to IN. Check that the device isn't `[OFF]` in **Devices** |
| A device has no address | It is disabled, or the controller has not run auto-addressing yet (e.g. power-cycle the HP-41 loop) |
| Nothing on `/dev/ttyACM0` | Check the `dialout` group and the port number. Early boot messages are buffered (4 kB) and sent when the terminal opens the port |
| New I2C device not found | **Config → Scan I2C**; check address, pull-ups and power |
| Files copied from the PC not visible | Select **Disconnect from PC** and eject on the PC before using the drive again |
| Build fails after editing `CMakeLists.txt` | Delete `build/` and configure again |
| `RP2350` drive does not appear | Use a USB cable that carries data, and hold BOOTSEL *while* connecting |
