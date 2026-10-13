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
├── PicoLED/                                ← submodule (see 1.2.1)
├── include/  src/  resources/  scripts/  pcb/
└── CMakeLists.txt
```

### 1.2.1 The PicoLED library (NeoPixels)

[PicoLED](https://github.com/ForsakenNGS/PicoLED) drives the WS2812 colour
LEDs ("NeoPixels") behind the `TFPIXEL` device. It is used through one file
only, `include/pixels.h` / `src/pixels.cpp` (`CPixelStrip`), and built as part
of the firmware -- there is nothing to install separately.

**Adding it** (if it isn't already there, e.g. after a clone without
`--recurse-submodules`, or to set up a new copy of the project):

```bash
cd ~/Projects/HIPI-Board
git submodule add https://github.com/ForsakenNGS/PicoLED.git PicoLED
git submodule update --init --recursive
```

Without git submodules a plain clone into the project folder works too:

```bash
cd ~/Projects/HIPI-Board
git clone https://github.com/ForsakenNGS/PicoLED.git PicoLED
```

**The folder name must match the build.** The build expects the library in
the project root under the name used in these three places -- keep them and
the folder name the same (Linux file names are case-sensitive):

| File | Line |
|------|------|
| `CMakeLists.txt` | `include("PicoLED/PicoLed.cmake")` |
| `CMakeLists.txt` | `PicoLed` in `target_link_libraries()` -- the CMake *target* name, defined by the library itself; stays as it is |
| `include/pixels.h` | `#include "../PicoLED/PicoLed.hpp"` |

**C++ exceptions.** PicoLED throws C++ exceptions when it can't get a PIO
state machine, so the project enables them with
`set(PICO_CXX_ENABLE_EXCEPTIONS 1)` in `CMakeLists.txt`. That line must come
*before* `pico_sdk_init()` -- the SDK reads it while initialising; set later
it has no effect, and `src/pixels.cpp` fails with
`exception handling disabled, use '-fexceptions' to enable`. After changing
it, run CMake again from an empty `build/` folder.

**PIO block.** PicoLED only accepts `pio0` and `pio1` (it throws
`InvalidPioBlock` for the RP2350's `pio2`). HIPI uses `pio1` for the strip
(`PIXEL_PIO` in `include/pixels.h`); `pio0` belongs to the HP-IL loop.
Because the strip is a global object, a wrong PIO block makes the firmware
stop before `main()` -- the Pico then seems not to start at all.

**Effects.** `TFPIXEL`'s effects (`E` commands, see the User Manual § 8.4)
are PicoLED's own effect classes -- `Comet`, `Marquee`, `Stars`, `Bounce`,
`Particles` (fire), `Fade` -- plus a rainbow written in `src/pixels.cpp`.
Each runs on a `slice()` of the strip (a `VirtualStrip` over just its
pixels), so several can run side by side; `CPixelStrip::poll()` in the main
loop animates them, at most every `PIXEL_FRAME_MS`. How many pixels are
connected (`N`) can't be detected: the controller says it, and it's kept in
`CONFIG.TXT` (`pixel_count`). `CPixelStrip::show()` limits the current
(`PIXEL_POWER_LIMIT_MA`) by lowering the brightness for a bright frame.
`tools/pixeltest` (`make test`) runs `src/pixels.cpp` with the real PicoLED
on the PC and checks all of it. Two PicoLED details: `PicoLedController::
getPixelColor()` is declared but not defined (pixels.cpp reads the colour
from the controller's target instead), and `Particles` frees its buffer with
`delete` instead of `delete[]` (harmless here).

**Colour order.** Strips take their colour bytes in different orders
(the board's: RGB; classic WS2812B: GRB; some WS2811: BRG ...), so it's a
setting: `pixel_order` in CONFIG.TXT, chosen on the Hardware test page
(**Order**), any of the six orders of R, G and B. `CPixelStrip::
setColorOrder()` changes the byte order in PicoLED's target at run time
(its `bytes[]`, protected: reached through a derived class, as the
controller's target for `getPixelColor()`) and writes what's in the buffer
again in the new order -- PicoLED itself is unchanged, and its effects
use the new order too. `PIXEL_COLOR_ORDER` in `include/pixels.h` is only
what the strip starts with. RGBW strips (SK6812 RGBW: 32 bits a pixel)
would need the PIO program set up for 32 bits; not supported.

**Updating** to a newer PicoLED:

```bash
cd ~/Projects/HIPI-Board/PicoLED
git pull
```

### 1.3 Open in VS Code

1. Install the **Raspberry Pi Pico** extension (Extensions panel → search
   "Raspberry Pi Pico").
2. Do **not** use *File → Open Folder* the first time. Instead:
   `Ctrl+Shift+P` → **Raspberry Pi Pico: Import Project**.
3. Select the folder containing `CMakeLists.txt`, board **pico2** (or
   **pico2_w**, see 1.4), SDK **2.2.0**.

The extension downloads the SDK/toolchain into `~/.pico-sdk/` if needed and
configures CMake for you.

### 1.4 Choosing the Pico board

HIPI runs on an **RP2350** board with ARM cores:

| Board | `PICO_BOARD` | Notes |
|-------|--------------|-------|
| Raspberry Pi **Pico 2** | `pico2` | The default in `CMakeLists.txt` |
| Raspberry Pi **Pico 2 W** | `pico2_w` | Works the same -- the wireless chip isn't used, and HIPI uses none of the pins it occupies (GPIO 23, 24, 25, 29) |
| Pico / Pico W (RP2040) | -- | **Not supported** |

**In VS Code:** `Ctrl+Shift+P` → **Raspberry Pi Pico: Switch Board** → pick
the board. The extension's choice overrides the default in `CMakeLists.txt`.

**On the command line:** pass it when configuring, e.g.
`cmake -DPICO_BOARD=pico2_w ..`.

CMake prints the result (`-- HIPI: board pico2, platform rp2350-arm-s`) and
stops with an error if the board isn't an RP2350 one. After changing the
board, configure again from an **empty** `build/` folder -- the board is
stored in CMake's cache.

> **Symptom of a wrong board:** copying the `.uf2` to the BOOTSEL drive
> seems to work, but the Pico doesn't restart -- the RP2350 ignores a
> firmware built for another chip. Check with
> `grep -E "PICO_BOARD|PICO_PLATFORM" build/CMakeCache.txt`
> (expect `pico2` or `pico2_w`, and `rp2350-arm-s`).

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

### 2.3.1 The version

The version is never typed in: every build takes it from git, from the
latest tag named `v<version>` (`cmake/hipi_version.cmake`, which writes
`build/hipi_generated/hipi_version.h`; only `src/version.cpp` includes it).

| The source is | The version shown |
|---|---|
| exactly on the tag `v1.2.0` | `1.2.0` |
| 5 commits after it | `1.2.0-beta.5` |
| on the tag, with uncommitted changes | `1.2.0-beta` |
| after a pre-release tag `v1.3.0-rc1` | `1.3.0-rc1`, then `1.3.0-rc1-beta.N` |
| without git and without a filled-in `VERSION` | `unknown` |

The splash screen, the boot screen and System → About show the short
form; the USB log adds the commit and the build date
(`HIPI Board v1.2.0-beta.5 (abc1234, 2026-10-12)`). hipiview gets its
version the same way (`hipiview --version`, the window title).

A release: `git tag v1.2.0 && git push --tags` -- the workflow builds that
tag, so the release's firmware says `1.2.0`. Everything built after it says
`1.2.0-beta.N` until the next tag. `HIPI-SD-card.zip`, which the build
remakes itself, doesn't count as an uncommitted change.

GitHub's *Source code* downloads have no git: there `git archive` writes
the version into the file `VERSION` (`.gitattributes`: `export-subst`),
which the build uses instead.

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
- **Menu:** On a running board, open **More → System → Bootsel mode**. The board
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

After reboot you should see the start-up screen with the version number,
followed by a short boot summary (USB status, current drive file, trace
mode, font, columns) and the device list.

---

## 4. Preparing the SD card

Everything a new card needs is in **`HIPI-SD-card.zip`** in the project
folder: format the card, unzip it onto the card's root, done. The zip is
made by `sdcard/make_sd_card.py` from `sdcard/` (README.txt, example
cassettes in `sdcard/lif/`) and the Tape view pictures in `resources/`. The
build (target `hipi_sd_card`, part of every build) remakes it whenever one
of those files changes, and the release workflow attaches it to every
release. The zip is reproducible, so git only sees a change when its
contents change. To write a mounted card directly:

```bash
sdcard/make_sd_card.py --to /media/$USER/HIPI
```

Format the card as **FAT32** or **exFAT**. Both work; cards larger than 32 GB are normally delivered as exFAT.
To reformat on Ubuntu (this erases the card):

```bash
sudo apt install exfatprogs
lsblk                              # find the card, e.g. /dev/sdb1
sudo mkfs.exfat -n HIPI /dev/sdb1  # or: sudo mkfs.vfat -F 32 -n HIPI /dev/sdb1
```

SD card layout (folder names in `include/sd_paths.h`):

```
CONFIG.TXT        settings -- created with defaults on first boot
README.txt        from sdcard/ -- what's what, for users
resources/        Tape and Printer view bitmaps, copied from the repo's resources/ folder
lif/              cassette images (*.dat, LIF), listed by Select file
screenshots/      screen dumps -- created on the first Screendump
logs/             analyzer logs -- created on the first Log to file
```

| File (in `resources/`) | Required | Purpose |
|------------------------|----------|---------|
| `hp82161a.bmp`  | No       | 7" panel only: picture for the **Tape** view. The view is left out if missing. |
| `tape-in.bmp`   | No       | 7" panel only: the cassette shown in the Tape view's lid window when a file is selected. |
| `open.bmp`      | No       | 7" panel only: the Tape view's lid area with the lid open, shown while the file picker is open. |
| `leds.bmp`      | No       | 7" panel only: the lit POWER and BUSY LEDs and the power switch in its STANDBY and ON positions. |
| `reels.bmp`     | No       | 7" panel only: rotated frames of the cassette's reel hubs, for the spinning-reel animation. |
| `hp82162a.bmp`  | No       | 7" panel only: picture for the **Printer** view. The view is left out if missing. |
| `prtparts.bmp`  | No       | 7" panel only: the Printer view's moving parts -- POWER lit, the switches in each position, the keys pressed. |

(`buttons.bmp` and `logo.bmp` stay in the repo's `resources/` only -- they're
built into the firmware as `src/buttons_image.cpp` and `src/logo_image.cpp`,
regenerated with `scripts/bmp_to_rgb565.py`.)

The button strip is built into the firmware (`src/buttons_image.cpp`), so
the buttons and menus work without an SD card. To change it, edit
`resources/buttons.bmp` and regenerate the source file:

```bash
scripts/bmp_to_rgb565.py resources/buttons.bmp src/buttons_image.cpp kButtonsImage
```

Once the board is running you can also copy files to the SD card over USB —
see **System → Connect to PC** below.

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

**Clock screen saver.** `src/clock.cpp` draws the time, date, weekday and
month with fonts pre-rendered from `resources/fonts/41charset_x.ttf` ("HP41 Character Set Xtended", the HP-41
display's own character shapes -- a symbol-encoded font, which the script
handles) by
`scripts/ttf_to_glyphs.py` into `src/clock_font_big.cpp` (250 pt, digits and
`:`) and `src/clock_font_small.cpp` (50 pt, A-Z, digits, `-/:,.`). Like on the
HP-41, `:` `.` `,` take no room of their own -- they're drawn into the gap
after the previous character (negative left bearing). 4-bit
anti-aliased glyphs, drawn by `drawGlyphText()` (`src/glyph_font.cpp`)
blended between the style's colours. To change font or sizes:

```bash
scripts/ttf_to_glyphs.py resources/fonts/41charset_x.ttf 250 "0123456789: " src/clock_font_big.cpp kClockFontBig
scripts/ttf_to_glyphs.py resources/fonts/41charset_x.ttf 50 "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-/:,. " src/clock_font_small.cpp kClockFontSmall
```

The time comes from a **DS3231** RTC module (`src/rtc.cpp`, address 0x68) on
the board's I2C bus -- the same bus as the touch controller and the BMP280:
SDA GPIO 16, SCL GPIO 17, 3.3 V and GND. The clock is shown as the
`DisplayOutput::Clock` view (no splash, not in the swipe order) by
`backlight_poll()` when **Settings → Screen saver → Mode** is Clock and the
RTC has a valid time (the backlight then fades down to `kDimLevel` over
`kClockFadeMs`, 4 s; any activity restores it at once); otherwise the
backlight dims. The time is set with
`time YYYY-MM-DD HH:MM[:SS]` on the USB debug console (CDC 0).

Until the DS3231 is fitted, `HIPI_FAKE_RTC` (`include/clock.h`, default 1)
replaces it with a stand-in clock: it starts at the firmware's build time,
counts with the Pico's timer, and can be set with the same `time` command
(lost at every restart). Set it to 0 once the RTC module is connected.

**Animated screen savers.** `src/saver_anim.cpp`, shown in the same screen
saver view as the clock (`clock_setShow()` picks what it shows):
*HP-IL rain* -- mnemonics falling in columns of `kSaverFontRain` (28 pt),
each column with its own speed and trail; the text comes from
`analyzer_nextFrameText()` (real frames seen on the loop) or a list of
made-up ones; at most `kColumnsPerStep` columns are drawn per main-loop
pass, so HP-IL isn't held up. *Goose* -- flocks of 1..5 of the HP-41 font's goose (0xFE), pre-rendered in
five sizes (`kSaverGoose40` .. `kSaverGoose112`, `src/saver_font_goose*.cpp`),
flying left to right at random heights (non-overlapping bands) and speeds;
a new group of 1..5 every 0.4..2.5 s while there's room (up to 8 at once). Clipped at the screen edges with
`drawBitmap565Cropped()`. *Random* (`SaverRandom`) picks one of `kRandomAnims[]` (`saver_anim.h` --
add new animations there) and Dark/LCD at random each time. In Clock mode
without a usable RTC,
`clock_fallback` (Settings → Screen saver → No clock) picks one of them, or Dim.

**Loop map.** `src/loopmap.cpp` reads the analyzer's capture with its own
cursor (`analyzer_nextFrame()`) and keeps one record per address: AAD passing
HIPI gives the addresses before HIPI (in-1), HIPI's range and, within it,
the PC side of PILBOX (addresses that aren't HIPI devices); TAD/LAD to
other addresses add devices after HIPI; SAI/SDI/SST answers fill in type,
ID and status. The layout (`layout()`) spreads the cards evenly over the
ring's top, right and bottom sides and picks full cards (<= 8), compact
cards (<= 16) or tiles (<= 30). Taps go to `loopmap_tap()` before the corner
checks in `boardui_handleTap()`.

**HP-IL signals view.** `src/hpil_diag.cpp` + `src/hpil_diag.pio`, on
**PIO2** + one DMA channel, only *reading* the input pins (no
`pio_gpio_init()` -- they stay PIO0's, the receiver isn't touched): while
the view is shown, the `hpil_scope` state machine samples both pins at 8 MHz
once either goes high and DMA fills a 32768-sample buffer (~4 ms); a new
capture every 0.5 s. The view (`DisplayOutput::Signals`) replays the
receiver's algorithm on the samples (plus = `IN_P_PIN`, minus sampled 0.5 µs
after plus ends) to show the bits it reads. Nothing runs while it's closed.

**PILBox CON.** In CON (`pilbox->mode() == CON`) `hipi_loop()` runs
`conLoop()` instead: HIPI is the frame's origin. A frame from the PC
(`CPilBox::conFromPc()`) goes through the devices after PILBOX in `devices`,
then out on HP-IL OUT (or, with `con_loop=internal`, straight on); when it
comes back (IN) it goes through the devices before PILBOX and to
`CPilBox::conReturned()`, which -- like the PIC firmware's `inIL` -- keeps a
returning command and puts an RFC round the loop, hands the command to the
PC when the RFC is back, and hands everything else to the PC. One frame at a
time; no return within 1 s, or a frame arriving with none of ours out,
gives a log line and a warning box (`boardui_showWarning()`). SSRQ (0x49C)
and CSRQ (0x49D) set/clear the SRQ bit HIPI puts in passing IDY frames in
COFF (and switch COFI to COFF), as in the PIC firmware.

**Menus.** `include/uidialog.hpp`: every menu is a `State::List` built by
`openList(title, items, back, sel)` from `Item`s -- a label function and an
OK action. `item()` makes a plain row (a trailing `" >"` draws the submenu
arrow), `value(name, valueFn, nextFn)` a setting row (`"name\tvalue"`,
drawn as `name ... < value >`; OK steps to the next value). OK with no menu
open calls `openContextMenu()`, which builds the menu of the view showing;
its "More >" leads to `openHipiMenu()` (Go to view / Settings / System).
The title goes in the frame's top band (`MenuFrame::TitleH`; drawn with
`kMenuTitleFont`, the HP-41 font, unless `HIPI_MENU_TITLE_HP41FONT` is 0).
The frame and title live in `include/menu_frame.h` (`MenuFrame::drawTitled()`
draws one at any place and size), so other dialogs look the same.
Order in every menu: where to go, what to do, settings, More.

**Printer paper by hand.** `paperOff_` in plotterview.cpp is how far the
paper is from where it prints (pixels; > 0 older lines, < 0 pulled up, at
most to the tear-off bar). The ink layer is moved with `shiftInk()` (BTE via
the staging area) and only the rows that come into view are rendered
(`renderInkRows()`), so dragging is smooth. The finger comes from
`touch_set_drag_callback()` (touch.h): a press that its Begin claims is
neither tap nor swipe. `pollPaper()` waits while the paper is held and
glides it back (`paperReturn_`) before anything new is printed.
Tearing off: at the edge (`paperOff_ == -kMaxPullUp`) a sideways move of
`kTearDx` sets `tearGesture_`; boardui takes it at the drag's end and opens
`UiDialog::openTearOff()` (State::TearOff: Save / Discard / Cancel, touch or
strip). Save is `printout_save()` (`src/printout.cpp`: 1-bpp BMP + text);
`plotterview_tearOff()` slides the ink sideways (`tearDir_`) and then calls
`CPrinter::clearPaper()`.
The menu's **Printouts** (`UiDialog::openPrintouts()`) lists
`printout_list()`; loading is `printout_load()` (the BMP read back into each
line's dots -- the 2x2 cells' top-left pixel -- and the text from the .txt)
into `CPrinter::loadPaper()`, then `plotterview_reloadPaper()` redraws the
ink at the live position. `printout_delete()` removes both files.

**Media map.** `src/mediamap.cpp` (DisplayOutput::MediaMap, from the Tape
view's menu): the LIF directory only (header, then entries up to the end
mark) becomes extents; a cassette gets its 2 tracks as rings, anything else
8 rings of `total/8` blocks, at most 400 steps a ring, each step showing the
most important kind in it (`kPriority`). Arcs are drawn as triangles
(`fillArc()`). The drive is reached through `mediamap_setSource()`
(pico_main.cpp): `CDrive::headBlock()/lastOp()/blocksWritten()` and
`CDrive::peekBlock()`, which reads through the drive's own open file and
puts its position back (FatFs locks a file open for writing). PC test:
`tools/hipiview/test/mediamap_test.cpp`.

**Create media.** The file picker's second row opens `State::CreateMedia`:
`MediaCreateUi` (`include/media_create_ui.h`) draws CREATE MEDIA and the
MEDIA NAME keyboard left of the button strip, and takes touches
(`UiDialog::handleTap()`, from `boardui_handleTap()`), buttons and polls
(`UiDialog::poll()`, from `boardui_poll()`). On the 7" panel the PIP window
follows the dialog's size (`UiDialog::setPip()`); on the 5" panel what's
behind it is redrawn before a smaller box (`eraseBigDialog()`). The image
itself is written by `LifCreator` (`include/lif_create.h`) -- laid out like
`lifinit -z` from lifutils, 32 kB per main-loop pass; a cancelled or failed
file is deleted. PC test: `tools/hipiview/test/media_create_test.cpp`
(the "SD card" is a folder, `test/pvstubs/ff.h`).

**Character table.** `src/chartable.cpp` draws the HP-41's codes 0..127 from
`src/chartable_font.cpp`, which `scripts/hp41_charset.py` generates from the
TrueType font **by HP-41 code**: the font is laid out like ASCII, with the
HP-41's special characters at 128+ (code 0, the overbar, is the font's 134;
code 1, x-bar, its 139 ...) -- the script's `KMAP` holds that mapping, and
draws the "starburst" (all segments) for codes the HP-41 can't show.

**Backlight dimming.** `src/backlight.cpp`: after `kDimAfterMs` (10 min,
`include/backlight.h`) without activity the backlight goes to `kDimLevel`
(~10 %). Activity = HP-41 text reaching the screen (`CDisplay::idle()`),
plotter drawing, the Tape view's lights changing, new analyzer lines, any
touch. Restoring uses `Screen::brightness()` (the menu setting); dimming only
changes the hardware. A touch on a dimmed screen only wakes it.

**HP-IL analyzer.** `src/analyzer.cpp` is a passive observer, not a
`CDevice`: `hipi_loop()` calls `analyzer_capture(in, out)` for every frame
(what arrived, what leaves -- `IL_NO_FRAME` if absorbed) into a 4096-entry
ring buffer (32 kB), and `analyzer_poll()` in the main loop analyses and
draws. The overview groups frames into transactions (controller -> listeners
data, talker -> controller answers after SDA/SST/SAI/SDI, auto-addressing,
IFC/DCL/LPD, the HP82161A's DDL/DDT commands decoded) and flags missing
answers, ETE and gaps over 500 ms (`kSlowUs`) inside a transaction; the
detailed mode is one explained line per frame. Changing mode re-analyses the
whole ring. Shown via `plotterview_showAnalyzer()` (`DisplayOutput::Analyzer`,
not in the swipe order); UiDialog routes OK/arrows to it while shown
(`AnalyzerMenu`). Logs go to `logs/analyzer_<n>.txt`.

**Start-up screen (7").** At power-up a full-screen overlay (PIP-1, drawn by
`src/bootscreen.cpp`) shows the version, a STATUS card whose rows are filled
in as start-up reaches each step (SD card, settings, USB, HP-IL, Tape images,
each drive's file, trace -- green = OK, red = missing, grey = off) and the
devices in loop order. It stays for 6 s (`kBootScreenMs` in `pico_main.cpp`)
or until the screen is touched. The 5" panel keeps the classic splash and
text summary.

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
| Horizontal swipe            | Switch view: each device with a view (display, plotter, every cassette drive) in loop order -- left = next device, right = previous |

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
│   ├── Hardware test      LEDs, pixels and the I2C bus (System -> Hardware test)
│   ├── Demo               HIPI shows what it can do (7" only)
│   └── Bootsel mode       Reboot into BOOTSEL for firmware update
├── Settings
│   ├── Emulate            HP 82163 / HP 92198
│   ├── Textcolor          White / Yellow / Green / Cyan / Red
│   ├── Font size          0 – 3  (HP 92198: 12x24 / 8x16)
│   ├── Brightness         20 % / 40 % / 60 % / 80 % / 100 %  (live preview)
│   └── Columns            Auto / 25 / 32 / 33 / 50 / 100  (HP 92198: 80 / 40)
├── Devices
│   ├── Enable/disable     Each HP-IL device on/off (OK toggles [ON]/[OFF])
│   └── Change order       Order of the devices on the loop
└── Display
    ├── Display            Show the text display
    ├── Plotter            Show the plotter canvas
    ├── Tape               Show the HP82161A tape drive (7" panel only)
    ├── Printer            Show the HP82162A printer (7" panel only)
    ├── Clear plotter      Erase the plotter drawing
    ├── Clear screen       Erase the text display
    ├── Analyzer           HP-IL bus analyzer view (own menu on OK)
    └── Screendump         Save the screen as screendump_<n>.bmp (7" panel only)
```

### 6.1 Config

- **Select file** — lists all `.dat` files in the SD card's `lif/` folder, with **No media**
  as the first row. The current file is pre-selected. Choosing **No media**
  deselects the file: the drive reports that no tape is inserted, the Tape
  view shows the drive without a cassette, and the info box and boot
  summary show "No media".
  After choosing a file, the confirmation dialog offers three choices:
  **OK** opens it, **X** goes back to the file list, and **▼** shows the
  contents of the LIF image: volume label, size, creation date, directory
  size, number of files and free blocks, followed by the directory (name,
  type, size -- in registers for HP-41 files, as the HP-41's DIR shows it
  -- allocated blocks, and S/P for secure/private). Scroll with ▲/▼
  (Shift for a page at a time); **OK** opens the file, **X** returns to
  the file list. After confirmation the drive (`TFDRIVE`) switches to the
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
- **Hardware test** — a page to try the LEDs (each one, a chase, HP-IL
  activity on LED 3/4 on or off), the colour pixels (count, a running light,
  solid colours) and the I2C bus (what answers, what it is, whether it
  works; **Scan I2C** shows the i2cdetect grid). See *Hardware test page*
  below.
- **Bootsel mode** — see [section 3.1](#31-enter-bootsel-mode).

### 6.2 Settings

- **Emulate** — which display `TFDISPLAY` is: HP 82163 or HP 92198 (80
  columns). Font size and Columns show the chosen one's settings.
- **Textcolor / Font size / Brightness** — apply immediately and are saved.
  Brightness previews as you move; **X** restores the previous level.
- **Columns** — forces the line-wrap width. *Auto* uses the natural width
  for the current font size; *32* reproduces the original HP82163 wrap
  width.

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

**Screendump** (7" panel only) saves what the panel shows as a 24-bit BMP
file, `screendump_<n>.bmp`, in the SD card's `screenshots/` folder. The menu itself, the
button strip and message boxes are not included. The number continues
from `screendump_next` in `CONFIG.TXT` (starting at 0), and numbers whose
file already exists are skipped. Saving takes a few seconds, during which
HP-IL isn't serviced, and isn't possible while **Connect to PC** is active.

A screen dump can also be requested from the HP-41 with the project's own
escape sequence `ESC # D` (screen only) or `ESC # M` (with menus, button
strip and message boxes -- exactly what's on the screen, mostly for
documentation). Send it to `TFDISPLAY`, e.g.:

```
27 ACCHR  35 ACCHR  77 ACCHR       ; ESC # M
```

The dump is taken right after, from the main loop.

**Choosing the view from the HP-41.** `ESC # V <addr>` (sent to
`TFDISPLAY`; `<addr>` is one byte, the HP-IL address 1..30) shows the view
of the HIPI device that currently has that address -- looked up in the
live device list at the time, so it follows auto-addressing, the device
order and the enabled/disabled state. If no enabled HIPI device has that
address, or the device has no view (`CDevice::viewKind()` is `None`, or
the view isn't available on this panel), nothing happens apart from a
warning on the debug log. E.g. the device at address 3:

```
27 ACCHR  35 ACCHR  86 ACCHR  3 ACCHR    ; ESC # V 3
```

Like the dump, the request is only recorded during HP-IL frame handling
(`plotterview_requestViewByAddress()`) and carried out from
`boardui_poll()` (`plotterview_showAddress()`) -- deferred while a menu is
open, and it ends the screen saver first. In `Screen`, commands that take
an argument byte are listed in the second parameter of
`setExtCommandCallback()` (currently `"V"`).

**Tape view.** Shows a picture of an HP82161A cassette drive. It is loaded
from `hp82161a.bmp` once at boot into a spare display layer, so switching to it
is instant. Tap the **cassette window** or the **OPEN** button to go
straight to the `.dat` file picker. Choosing a file, or pressing **X**,
returns to the Tape view. When a `.dat` file is selected, a cassette with
the file name on its label is seen through the lid window; while the file
picker is open (from the Tape view or from **Config**) the lid is shown
open with the drive empty. The **POWER** LED is lit while `TFDRIVE` is
enabled (**Devices** menu), and **BUSY** follows the owner's manual: it is
lit while the drive executes an operation (reading, writing, seeking,
formatting, rewinding) or is addressed as talker or listener. The
cassette's reels only spin while the tape actually moves. The power
switch works like the real OFF-STANDBY-ON switch: each tap moves it one
step (OFF -> STANDBY -> ON -> STANDBY -> OFF). OFF disables `TFDRIVE`
(once it's idle); ON keeps it powered; in STANDBY the HP-41 can power it
down with Loop Power Down (POWER light off) and it wakes up again on the
next HP-IL activity. The position is saved in `CONFIG.TXT`.

Tapping **REWIND** rewinds the tape to its start, as on the real drive
(ignored while BUSY is lit, and when the tape is already at its start). The
drive then stays busy for 2 to 10 seconds, depending on how far into the
tape it was (2 s just after the start, 6 s halfway, 10 s at the end): BUSY is
lit, the reels spin backwards, and the HP-41 gets the drive's Busy status
until it's done.

The Tape view is not available on the 5" panel, which has no spare display
memory.

**Printer view.** `TFPRINT` (`printer.h`/`.cpp`) is an HP 82162A thermal
printer, and its view a picture of one: `hp82162a.bmp`, made to match
`hp82161a.bmp` in size, place and colour (the real devices are the same
size and stack). It's loaded once at boot into its own layer
(`LT7683::kPrinterLayerAddr`, the 7th 1024x600 slot -- past CGRAM's,
inside the 16 MB SDRAM), its moving parts (`prtparts.bmp`) into
`kPrinterPartsAddr`. Each part is a piece of the same picture with one
control changed, so showing it is one BTE copy over the base picture:
POWER lit and the switch ON, the MODE switch's three positions, the
INTENSITY switch's five, PRINT and PAPER ADVANCE pressed -- and the
print head, which moves.
`plotterview_poll()` compares the printer's state with what's drawn and
copies only what changed. Both pictures are made from a photo by
`tools/images/make_hp82162a.py`, which also prints the sprite table that
`kPrt*` in `plotterview.cpp` must match.

Touches (`plotterview_printerHitTest()`, handled in `boardui.cpp`): the
power switch toggles `TFPRINT` on and off (like **Devices**), MODE and
INTENSITY jump to the position touched (or one step for LIGHT/DARK), PRINT
and PAPER ADVANCE are held while the finger is down
(`boardui_handleRelease()` lets go) and shown pressed for at least 150 ms.
The positions are saved (`printer_mode`, `printer_intensity`). Unlike the
other device views, the Printer view is there while the printer is off:
it starts off, and its switch is in the view.

On the loop the printer answers like the real one (and pyILPER's
emulation): accessory ID 0x20, address 1 after AAU, two status bytes on
SST (low byte first: SR, MB = NORM, MA = TRACE, PA, PR; high byte: EL, ID,
BE, EB, RJ, DW, CO, LC), then ETO. A key press sets SR, and while it's
set the printer adds the Service Request bit to passing data and IDY
frames; SST clears it. A quick tap is still reported once (PR/PA stay in
the status until read, at most 3 s).

A line is built like the real printer does: 168 dot columns (24
characters of 7: a blank, the 5x7 glyph, a blank), from characters
(`printer_font.cpp`: ASCII in escape mode, the HP-41's set in 8-bit mode
-- the glyphs of pyILPER's emulation), graphics columns (column mode, ESC
*b<n>G), skips, double wide and lower case; it's printed at CR, when the
next item doesn't fit, right justified if asked. Bar codes are consumed,
not drawn. The last `kMaxLines` (200) lines are kept, as dots and text
(`CPrinter::lines()`, `line(n)`); with trace on each is in the log.

**The paper.** The ink is kept in its own layer (`kPrinterInkAddr`): the
print area as seen through the window, 252 x 323 pixels, ink on white,
the newest line at the bottom. Dots are 1.5 px (168 -> 252), anti-aliased,
blue-black, as dark as INTENSITY says when the line is printed; a line is
16 px. On the panel the ink is combined with the picture's own paper by a
BTE "S0 AND destination" (`LT7683::andLayerRegion()`, ROP 1000b): white
leaves the picture as it is, so its shading under the cover stays; the
tear-off bar's rows are left out. Below the bar the paper bends towards
the print head and runs a little to the left (`kSlant`, up to 8 px at the
lower edge, a gentle curve -- the picture is made with the same table), so
the ink there is put on in bands of rows, each moved by its row's slant.
A new line, as on the real printer: the
print head -- the small carriage in the slot under the paper, a sprite
(`kPrtHead`, cut from the photo; the picture has the slot without it) --
glides over to the other side (12 steps, eased), while the line is put
into the ink layer one line *below* what the window shows (the layer is
`kInkBufH`, a line taller); then the layer moves up 4 px at a time (via a
staging copy, `kPrinterInkStageAddr`) and the line comes into view. The
next line the head glides back. Blank lines (PAPER ADVANCE, an empty CR)
only move the paper. One step per `plotterview_poll()`, at most every
15-20 ms, so the main loop isn't held up; while the head moves only the
slot is redrawn. Lines that come
faster, or while the view isn't showing, go onto the paper at once;
more than a window full redraws it from `line(n)`. PAPER ADVANCE held
feeds a line every 220 ms.

`tools/printertest` checks the HP-IL side and the dots (`make test`);
`tools/hipiview`'s `printer_view_test` runs the real `plotterview.cpp`,
`printer.cpp`, BMP loader and LT7683 driver against the emulated chip
with the real pictures, checks the paper and saves `printer_view_*.png`.

**The display's two models.** `TFDISPLAY` (`CDisplay`, `display.h`/
`.cpp`) is one device that emulates either display -- **Emulate** in
Settings → Screen and in the Display view's menu, `display_model` in
CONFIG.TXT, `CDisplay::setModel()`:

- HP 82163: accessory ID 0x3E, the bytes go to `Screen`; SST/SDA are not
  answered (the frame passes on).
- HP 92198 (Mountain Computer MC00701A, 80 columns): accessory ID 0x32,
  device ID `MC00701A` CR LF (`deviceId()`), SST = 0x80 then the device
  dependent status byte (bit 0 40 columns, 1 Roman-8, 2 PAL, 3 cursor
  off, 4 insert mode), then ETO; DCL/SDC is a hard reset. The bytes go to
  `hipi::Video80` (`video80.h`/`.cpp`, made in `pico_main.cpp`).

Both: address 31 after AAU, data queued and handed over one byte per
`idle()`. The controller sees a changed identity the next time it sets
up the loop. On the panel the Display view is `Screen` (output
`DisplayOutput::Display`) or `Video80` (output `DisplayOutput::Video80`):
`outputFor(ViewKind::Display)` and the "back to the display" paths ask
`displayOutput()`, and `plotterview_setDisplayModel()` swaps them while
the display is showing (on the 5" with the menu open it only hands over
-- closing the menu draws). Text colour applies to both; each keeps its
own font and columns settings (`fontsize`/`columns` for the 82163,
`v80_font`/`v80_columns` for the 92198), and the menu's **Font size** and
**Columns** show the current model's.

`Video80` is separate from `Screen`, after the owner's manual:

- 48 lines of memory, a window of 24 (or 20 in 40-column mode) shown.
  LF on the window's last line moves the window down (clearing the line
  coming in) until it reaches the end of the memory; then the whole
  memory moves up and line 0 is lost. ESC S / ESC T move the window.
- Cursor addressing (ESC %, A–D, H) is relative to the window, clamped
  to the screen -- no wrap at 16 rows / 32 columns as on the 82163.
- Long lines continue on the next line (`cont_[]`); insert (ESC N, a new
  line is made when needed) and delete character (ESC O) work across the
  whole long line. ESC J/K/L/M, ESC Q/R, ESC Y/Z (monitor mode: control
  codes shown in cyan as `@`–`_`), ESC [ / ] (80×24 / 40×20, clears), ESC
  / and = (NTSC/PAL, only the status bit), ESC e/E, ESC h, ESC j/k, ESC z
  (always passes) and ESC > / <. HIPI's own `ESC #` / `ESC ~` commands
  (screen dumps, choosing the view) work as on `TFDISPLAY`.
- ASCII set: 128–255 are the same characters in inverse video. Roman-8:
  bytes kept as they are, drawn as the nearest ISO 8859-1 character
  (`kRoman8[]`), the solid block as an inverse space.

It draws with the chip's text engine and its built-in font: 12×24 on the
7" panel (`selectBuiltinFontSize(1)`, 80×24 = 960×576) unless the small
one is chosen (`setSmallFont()`), 8×16 on the 5"; 40 columns doubles the
width (`txtScaleXY(2, 1)`); `setDefaultForty()` is the width after a
reset. Changes draw only the
cells or lines that changed; LF at the bottom is one `bteScrollShift()`
plus the new line (7"). The chip's text cursor is the cursor: a block, or
an underline for the insert cursor (`setTextCursorShape()`). The view only
draws while it's shown (`setActive()`), and not while something has
pointed the drawing elsewhere (the 7" menu) or covers it (5" menu/strip):
then it remembers to draw everything (`catchUp()`). When it stops it puts
the 8×16 font and scale back for the other views; the info box and device
list select the built-in font themselves.

As an HP 92198, ▲/▼ (shift: a page) look back through the memory, X goes
back to the cursor, Shift+X clears the screen.

`tools/hipiview`'s `video80_view_test` runs `video80.cpp`, `display.cpp`
and the LT7683 driver against the emulated chip, checks the HP-IL answers
of both models, the escape sequences, scrolling, long lines and the font
and columns settings, and saves `video80_*.png`.

**Hardware test page.** `hwtest.h`/`.cpp` is a full-screen view
(`DisplayOutput::HwTest`, `plotterview_showHwTest()` from System, X or
**Leave view** goes back) drawn like the loop map: off-screen in layer 3
and copied on the 7", directly on the 5"; small changes (a button, a pixel
dot, the I2C rows -- padded text, no clearing) are drawn on the panel when
nothing else owns the canvas, else the whole page is redrawn later. Touches
go to `hwtest_tap()` (boardui.cpp), `hwtest_poll()` runs the LED chase
(150 ms), the pixel run (60 ms a step) and the I2C rescan (3 s).

The hardware is behind `hwtest_io.h` (firmware: `hwtest_io.cpp`; PC test:
`tools/hipiview/test/hwio_stub.cpp`). `hwio::begin()` holds the HP-IL
activity LEDs (`hpil_leds_hold()`), switches the LEDs off and stops
`TFPIXEL`'s effects; `hwio::end()` switches everything off again. The
pixel count is `pixelStrip.setActiveCount()` -- TFPIXEL's saved
`pixel_count`. HP-IL activity (`hpil_led_activity()`, leds.cpp) flashes
LED 3 for 30 ms per frame (restarted at most every 10 ms) and keeps LED 4
lit while no frames come, through the PWM LED drivers -- the old
`led_on(gpio)` did nothing on those pins since they are PWM outputs.
`hpil_leds=0` leaves them alone.

The I2C list is `i2c_identify()` (`i2c_ident.h`/`.cpp`): every address
0x08-0x77 is probed with a one-byte read (3 ms timeout), then each one that
answers is matched against the chip descriptions. Those are text files:
the user's `.txt` files in `i2c/` on the SD card, read first (so they win),
then always the built-in `sdcard/i2c/chips.txt` (`kI2cChipsDefault`,
generated by CMake from `src/i2c_chips_default.cpp.in` -- changing
`chips.txt` re-runs CMake); all read again each time the page opens
(`i2c_loadChips()`). A `chips.txt` on the card is not read: an old copy
there would hide what a newer firmware knows (it once did: the VL53L0X's
`MD` answered ERR NAME from a stale card copy). The SD card zip has it as
`i2c/chips.txt.example` (`make_sd_card.py`). The format and the matching are in
`i2c_chips.h`/`.cpp` (no hardware: the bus is an `I2cBus`): a `chip` with
an address range, a name and what it is, then checks -- `id`/`id16` (a
register's value, optionally renaming the chip), `text`, `status` (a byte
read without a register), a `show` reading with a `range`, and `probe
<name>` for the checks that need code, which stay in `i2c_ident.cpp`:
`touch`, `clock` (DS3231/DS1307: valid BCD time, ticking, temperature,
"oscillator stopped") and `bosch` (the chip ID; a BMP280 on 0x76 or 0x77
is read through `CBmp280` for temperature and pressure). Chips with checks are tried before those
without ("probably"), each group in reading order (files by name).
`expected` chips (the panel's touch controller, `panel 7`/`panel 5`) show
as missing when silent. Only register numbers are written, and never on
0x20-0x27 (a PCF8574 would take it as its output levels): there only
`status` and `probe` checks run. Lines that can't be read go to the log,
and the page's I2C header says what was read. `tools/i2ctest` checks the
format with `chips.txt` itself and the matching on a made-up bus.

**TFI2C** (`ili2c.h`/`.cpp`, last on the loop so the others keep their
addresses) lets the controller use any I2C chip. The device only collects
a line (ended by CR/LF or UNL) and sends the answer (then CR LF, ETO); the
commands are `I2cCommander` (`i2c_command.h`/`.cpp`, no hardware):
`A` (address, or a chip name via `i2c_identify()`), `R`, `W`, `Q`, `D`,
`M` (measurement), `L`, `P`, `S`, `I`, separated by `;` or by a `,` that
a letter follows (`A118,MT`: the HP-41 hardly types `;`). Numbers in: decimal or `$hex`;
out: decimal. A line without `W` is kept and run again at every SDA (a
fresh value per INA); a line with `W` runs once when it ends.
Waits run inside the frame handling (at most 2 s per line) -- the
controller simply waits for the frame, as with a slow device. Writes to
HIPI's own chips are refused (`hipiOwns()`: the touch panel). DCL reads
the chip files again.

Measurements are described in the chip files (`i2c_chips.h`): a
`measure` block with `write` / `wait` / `read [reg] n [crc=..]` steps and
a `value` (bytes to a number: mask, sign, shift, scale, offset), `format
bcd` (with `text=` pieces: the DS3231's `TIME` as HH.MMSS) or `builtin`
(`bosch.T`/`bosch.P` through `CBmp280` -- the Bosch chips need their
calibration maths; `vl53l0x.D` -- ST's long set-up, `src/vl53l0x.cpp`); `setup` steps run once per
address before the first measurement. `I2cChipList::run()` /
`measure()` do the work; CRC: Sensirion (per word), AHT (all, from 0xFF),
HTU (all, from 0). (TFI2C replaced the old TFTEMP device: `MT`, `MP`,
`P<n>` are the same.) `tools/i2ctest` covers the measurements, the
VL53L0X set-up on a made-up chip, every command and the device on the loop.

`tools/hipiview`'s `hwtest_view_test` runs the page (with the stand-in
hardware and a made-up bus), taps its buttons, checks the LEDs and pixels
it drives and saves `hwtest_*.png`.

**Demo.** `demo.h`/`.cpp` (System → Demo, 7" only) is a script of steps
run from `demo_poll()`: each step is a function called every poll until it
says it's done (`Step`), built by helpers -- `type()` (characters to
`Screen` or `Video80`, n ms apart), `hpgl()` (HP-GL to the plotter, n
characters per 15 ms, through `CPlotter::put()`), `print()` (lines to
`CPrinter::put()`), `view()` (`plotterview_showView()`: a view without the
name box), `caption()` (a yellow band at the bottom, drawn in the menu
layer and shown as PIP-1), `wait()`, `call()`. What is shown lives in
`demo_content.cpp` (texts, HP-GL, printouts; plain data -- the logo is
hatched from the built-in bitmap, one stroke per run of coloured pixels in
the nearest pen).

Special steps: `tapePick()` opens the lid (`plotterview_setTapeEjected()`),
shows the cassette list through UiDialog's own drawing
(`demoShowPicker()`/`demoMovePicker()`/`demoHidePicker()`, via
`DemoHooks` -- nothing is chosen, the menu stays closed), puts the picked
name on the label and lets the reels turn (`plotterview_setDemoTape()`);
`dir()` formats `lifDescribe()` of that file. `session()` sends a made-up
HP-41 session through HIPI's own devices with `hipi_simFrame()` (the same
path as a real frame, minus PILBOX and the PIO; the analyzer captures it,
so the analyzer and the loop map show it) -- the devices' addresses are
saved first and put back when the demo stops. `lightShow()` uses
`hwtest_io.h`.

Stopping (`demo_stop()`): any touch, swipe or hipiview button
(boardui.cpp), or a real non-idle HP-IL frame (`demo_hpilActivity()` from
`hipi_loop()`, acted on at the next poll). It puts back what it changed
(Screen's size/colour/columns, Video80's font/width/colour, plot fit and
paper, the Tape view's label, the display model's view, the addresses),
empties the display, the 92198 screen, the plot, the paper
(`plotterview_clearPaper()`) and the analyzer (`loopmap_reset()` too), and
shows the view it started from. Nothing is written to CONFIG.TXT.

`tools/hipiview`: `demo_view_test` renders `demo_content.cpp` (the texts,
the plots, the printout) through the real Screen, Video80, plotter and
printer; `demo_run_test` runs the real demo engine for a whole round in
fake time (stand-ins for the SD card, analyzer, LEDs and the loop), saves
a picture of each view and checks that stopping puts everything back.

**New devices and CONFIG.TXT.** `known_devices` lists the devices the file
has met. In `hipi_init()`, a device not in it (new in this version) gets
its first state and is added: the printer starts switched off, since with
a printer on the loop the HP-41 prints there instead of on the video
display.

**Devices** has two items: **Enable/disable** (the list above) and
**Change order**, which sets the devices' order on the loop (and so
the order in which the HP-41 addresses them): pick a device with ▲/▼ + OK,
move it with ▲/▼, OK keeps the new position, X puts it back. The order is
saved as `device_order` in `CONFIG.TXT`; without that line the default order
from `hipi_init()` applies.

---

## 7. USB interfaces

With USB connected, the board enumerates as a composite device:

| Interface | Linux device      | Purpose |
|-----------|-------------------|---------|
| CDC 0     | `/dev/ttyACM0`    | Debug console / trace log |
| CDC 1     | `/dev/ttyACM1`    | PILBox link to a PC emulator (e.g. pyILPER) |
| CDC 2     | `/dev/ttyACM2`    | `TFTERM` terminal bridge (e.g. HP-71B `DISPLAY IS`/`KEYBOARD IS`) |
| MSC       | USB drive         | SD card, only while **Connect to PC** is active |
| CDC 3     | `/dev/ttyACM3`    | Display mirror for `tools/hipiview` (7" panel only) |

Device numbering may differ if other CDC devices are connected. To read the
debug log:

```bash
minicom -D /dev/ttyACM0     # or: screen /dev/ttyACM0
```

If your user cannot open the ports, add yourself to the `dialout` group
(`sudo usermod -aG dialout $USER`, then log in again).

### 7.1 Display mirror (CDC 3)

CDC 3 ("HIPI Display Mirror") carries a copy of everything the firmware
sends to the LT7683, so `tools/hipiview` can show and record the 7" display
on a PC (see its [README](../tools/hipiview/README.md)). It works at the
register level, so new drawing code needs nothing extra to be mirrored.

- `MirrorTransport` (`display_mirror.h`) wraps `PicoSpiTransport`: every SPI
  transaction is passed on unchanged and fed to `mirror::Encoder`
  (`mirror_encoder.hpp`). The encoder always keeps a shadow copy of the
  registers. Only while the port is open does it produce packets
  (`mirror_protocol.h`): register selects, data writes, and the data of
  MRWDP (memory) reads. Status reads and other register reads are not sent.
- USB loses a packet now and then, so the link is reliable. Packets go in
  numbered frames of at most 512 bytes (`0xA5 0x5A sid len pos crc`) into
  a 32 kB ring buffer. A frame stays there until hipiview acknowledges it
  ('K'). hipiview asks for a resend from where something went missing
  ('N'). With no acknowledgement for 300 ms, HIPI sends again from the last
  acknowledged frame, which covers a lost last frame.
- The ring is drained to the USB port from the main loop by
  `displayMirror_poll()`. `tools/hipiview` `make link_sim` tests the real
  `display_mirror.cpp` against hipiview over a USB that loses packets. If the ring is full, drawing waits
  for USB: at most 1 s, and at most 0.5 s without a word from the viewer.
- A viewer that stops reading with the port still open (hipiview suspended
  or hung, the PC asleep) ends the session: the ring stayed full, or
  nothing was acknowledged and nothing heard for 3 s. HIPI then stops
  mirroring and waits for a new request; it never starts over by itself,
  which would stall its main loop against a port nobody reads
  (`./link_sim 0.01 20 stall` tests this). When idle, HIPI sends an empty
  frame every second; hipiview acknowledges it, and asks for a new session
  if it hears nothing for 3.5 s.
- The setting **Settings → Screen → Mirror to PC** (`display_mirror=off|view|control`,
  off by default, `displayMirror_setMode()`) gates it all: off, requests
  are ignored (one log line) and a running session ends at once, freeing its
  buffers. Switched on again while the viewer is still connected, a session
  starts by itself with the picture ids from the viewer's last request.
- **Control:** the viewer also sends input -- `'T' sid x y state` (the mouse
  as a finger, panel pixels) and `'B' sid code state` (a button; see
  `mirror_protocol.h`). `display_mirror.cpp` passes them, only in Control and
  only from the running session, to the handlers set with
  `displayMirror_setInputHandlers()`: `touch_remote()` (`touch.cpp`), a
  second finger that goes through the same debouncing and gesture detection
  as the real one, and `boardui_remoteButton()`, which presses the strip's
  buttons (sliding it in first). The viewer repeats a held touch or button
  every 200 ms; without news for 1 s HIPI lets go by itself, and the end of
  the session lets go of everything. hipiview holds a click at least
  100 ms, so the 60 ms debouncing sees it. The input is queued and handed
  to the handlers only from the top of `displayMirror_poll()`: the viewer is
  also read while the ring is full, from inside a drawing
  (`RingSink::put()`), and a handler that draws (a button opening a menu)
  must not run there -- it did once, and HIPI crashed.
  `./link_sim 0.01 18 input` tests the whole chain, that too.
- A session starts when the viewer sends `HIPIVIEW` plus the ids of the
  pictures in its cache. Just opening the port (ModemManager, terminals)
  starts nothing. `mirror::Session` (`mirror_session.hpp`) then sends a
  greeting, the register snapshot (plus the live cursor positions), and
  the chip's built-in fonts (`kFont` 8×16, `kFont24` 12×24 -- protocol
  version 6). `LT7683::captureBuiltinFont()` / `captureBuiltinFont24()`
  draw them off-screen and read them back, once per boot.
- Then it reads back, 8 kB per main-loop pass, with `LT7683::readSdram()`
  (the encoder suspended, the data sent as `kMem` packets, the changed
  registers resent afterwards), in this order:
  1. the assets the viewer doesn't have yet;
  2. the run-time layers and the CGRAM font;
  3. what the PIP windows show;
  4. the panel, bottom row first.

  HIPI keeps drawing meanwhile, and block transfers copy from SDRAM -- from
  a picture to the cassette label, from the layers onto the panel, within
  the panel when text scrolls. A copy from an area the viewer doesn't have
  yet would give it garbage (it once left the cassette black in the Tape
  view), so sources come before what is copied to, the panel comes last,
  and it goes bottom up (a scroll up then only copies rows the viewer
  already has). `make test` draws, copies and scrolls during the read-back.
- Off-screen layers are registered where they're loaded, with
  `displayMirror_addLayer()`. The Tape pictures are *assets*, with an id from
  `displayMirror_fileAssetId()` (file name, size, date, position). The button
  strip and the cassette label change at run time and are always read back.
  A new layer that a viewer needs must be registered the same way.
- Drawing in between is mirrored as usual, so the order is always right.
- `CFG_TUD_CDC_EP_BUFSIZE` (512) must not exceed `CFG_TUD_CDC_RX_BUFSIZE`
  (`tusb_config.h` checks it), or TinyUSB never arms the receive endpoints
  and no CDC port can receive.
- `tools/hipiview/test` runs the real driver, `Screen`, encoder and session
  against an emulated chip and checks that the viewer ends up identical
  (`make test`).

---

## 8. Built-in HP-IL devices

The devices are chained in this order, which is also their order in the
HP-IL loop (see `hipi_init()` in `src/hipi.cpp`):

| Name        | Class        | SAI  | Function |
|-------------|--------------|------|----------|
| `TFDISPLAY` | `CDisplay`   | 0x3E / 0x32 | HP82163 video display, or HP 92198 / MC00701A 80 columns (setting) |
| `TFDRIVE`   | `CDrive`     | 0x10 | HP82161 cassette drive, backed by a `.dat` on SD |
| `TFLEDS`    | `CHipiLed`   | 0xEE | LED control (syntax in `README.md`) |
| `TFPIXEL`   | `CHipiPixel` | 0xED | Pixel/LED strip control |
| `TFI2C`     | `CHipiI2c`   | 0x52 | Any I2C chip: registers, or measurements from `i2c/*.txt` (last on the loop) |
| `PILBOX`    | `CPilBox`    | —    | Forwards the loop to a PC over CDC 1 |
| `TFPLOT`    | `CPlotter`   | 0x60 | HP7470A-compatible plotter (HP-GL subset) |
| `TFTERM`    | `CTerminal`  | 0x4E | Terminal bridge over CDC 2 |
| `TFPRINT`   | `CPrinter`   | 0x20 | HP 82162A thermal printer (starts off) |

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
5. **Debug** with **Settings → PC link → Trace: Extended** and the log on
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

### 9.5 Example: an I2C sensor

An I2C sensor doesn't need an HP-IL device of its own: `TFI2C` reaches
every chip on the bus, and what a measurement is lives in the chip files
(`i2c/*.txt` on the SD card, see *Hardware test page* and the top of
`sdcard/i2c/chips.txt`). In order of preference:

1. **A description only** -- most sensors: write a register, wait, read
   bytes, scale them. No firmware change:

   ```
   chip 0x23 BH1750 | light
     measure L,LUX | light in lux
       write 0x20                 # one high-resolution measurement
       wait 180
       read 2
       value bytes=0-1 scale=0.8333 dec=0
   ```

2. **A built-in measurement** -- when the chip needs code: calibration
   maths (the Bosch sensors) or a long set-up (the VL53L0X). Write a
   function that measures through the `I2cBus` (so it runs in
   `tools/i2ctest` too), register it in `i2c_commandEnv()`
   (`src/i2c_ident.cpp`) and name it in the chip file:

   ```cpp
   // src/my_sensor.cpp -- one value, set-up done when needed
   bool mysensor_read(hipi::I2cBus& bus, std::uint8_t addr, double& value);

   // src/i2c_ident.cpp, i2c_commandEnv():
   e.measures = { ..., { "mysensor.X", mysensor_read } };
   ```
   ```
   chip 0x52 MYSENSOR | something
     measure X | the value
       builtin mysensor.X dec=1
   ```

   `vl53l0x.cpp` is a full example: it does ST's set-up the first time,
   notices when the chip has lost it (unplugged: a register is back at
   its reset value) and does it again; `vl53l0x_reset()` (called on
   DCL via `i2c_loadChips()`) forgets every set-up.

The HP-41 then only says which chip and which measurement:
`"AMYSENSOR;MX" OUTA INA`.

**The shared bus.** All I2C chips share `i2c0` on GP16 (SDA) / GP17 (SCL)
with the touch controller. `touchInit()` initialises the bus and
`pico_main.cpp` then calls `CI2CBus::markInitialized(touch_i2c)`, so a
driver's `initBus()` doesn't initialise it again. Use **System → Hardware
test** (its I2C list, or **Scan I2C**) to see the chip's address first.

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
3. Give the device a view: add a value to `ViewKind` (`include/hpil.h`)
   and to `DisplayOutput` (`include/plotterview.h`), return it from the
   device's `viewKind()`, and handle it in `plotterview.cpp` (`outputFor()`,
   `plotterview_viewTitle()`, `switchView()`, and `plotterview_isAvailable()`
   if the view isn't available on every board). Swiping and the **Display**
   menu then pick it up automatically, in loop order.
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
| The `.uf2` copies to the BOOTSEL drive but the Pico doesn't restart | Built for the wrong board -- see 1.4 (needs `pico2` / `pico2_w`), then rebuild from an empty `build/` |
| "PANIC: f_mount error" in the log | SD card not inserted / not formatted / bad contact |
| HP-IL doesn't respond | Run **System → Loopback test** with a cable from OUT to IN. Check that the device isn't `[OFF]` in **Settings → Devices** |
| A device has no address | It is disabled, or the controller has not run auto-addressing yet (e.g. power-cycle the HP-41 loop) |
| Nothing on `/dev/ttyACM0` | Check the `dialout` group and the port number. Early boot messages are buffered (4 kB) and sent when the terminal opens the port |
| New I2C device not found | **System → Hardware test** (I2C list / Scan I2C); check address, pull-ups and power |
| Files copied from the PC not visible | Select **Disconnect from PC** and eject on the PC before using the drive again |
| Build fails after editing `CMakeLists.txt` | Delete `build/` and configure again |
| `RP2350` drive does not appear | Use a USB cable that carries data, and hold BOOTSEL *while* connecting |
