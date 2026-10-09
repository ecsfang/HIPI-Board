# hipiview — HIPI's display on the PC

`hipiview` shows HIPI's 7" display in a window on the PC, live, and
records it straight to a video file. It's useful for making videos: no
camera, no reflections, no lighting to get right. Every pixel is exactly
what the panel shows, at the panel's own 1024 × 600.

It works with the **7" panel** (LT7683) only.

## How it works

First switch the mirror on in HIPI: **Settings → Screen → Mirror to PC**
(saved; **Off** from the start):

| Setting | hipiview |
|---------|----------|
| **Off** | gets nothing; the title bar says "no answer from HIPI" |
| **View** | shows (and records) the screen |
| **Control** | ... and the mouse and keyboard work HIPI too (see [Controlling HIPI from the PC](#controlling-hipi-from-the-pc)) |

HIPI has a fourth USB serial port, **HIPI Display Mirror** (CDC 3). Once
hipiview has opened it and asked for the screen, HIPI sends everything it
sends to the display controller to that port as well. hipiview runs an emulated LT7683 on that
stream: SDRAM layers, text engine, drawing engine, block transfers, PIP
windows (the menu and the button strip), the blinking cursor, and so on.
The picture is therefore built exactly the way the real chip builds it, and
anything added to the firmware later is mirrored too.

When hipiview connects, HIPI first sends its current state: the register
values, the chip's built-in font, and the contents of the panel and the
off-screen layers. The pictures and layers come first and the panel last
(HIPI may copy from them onto the panel while this goes on, so they must be
there first). The title bar shows the progress, and HIPI
keeps working normally meanwhile. When hipiview closes the port, mirroring
stops and HIPI no longer spends any time on it.

**Picture cache.** The Tape view's pictures (from HIPI's SD card) are sent
only the first time. hipiview keeps them in `~/.cache/hipiview/`, and later
sessions only name them. If you replace a picture on the SD card, its new
version is sent automatically, because a picture's id comes from its file
name, size and date. Every cached picture carries a checksum: a damaged
one (or one from an older hipiview) is thrown away and simply sent again.
You can delete the folder at any time; it just fills up again.

## Building

### Linux (Ubuntu)

```bash
sudo apt install build-essential libsdl2-dev zlib1g-dev ffmpeg
cd tools/hipiview
make
```

`ffmpeg` is only needed for recording.

### Windows

**Ready-made:** every HIPI release on GitHub has
`hipiview-windows-<version>.zip`: one `hipiview.exe`, with SDL2 and zlib
built in (and `hipiview-linux-x64-<version>.tar.gz` for Linux). The
workflow *Build hipiview* (`.github/workflows/build-hipiview.yml`) makes
them; it can also be run by hand from the Actions tab, and then the files
are under *Artifacts* on the run's page. Unpack it and run it from a
command prompt (`cmd` or PowerShell), so its messages can be seen:

```
hipiview.exe                       finds HIPI's mirror port by itself
hipiview.exe COM7                  or name it
```

**Built on Windows**, with [MSYS2](https://www.msys2.org) (UCRT64 shell):

```bash
pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake \
          mingw-w64-ucrt-x86_64-SDL2 mingw-w64-ucrt-x86_64-zlib
cd tools/hipiview
cmake -B build -S . && cmake --build build
```

`build/hipiview.exe` then needs `SDL2.dll` and `zlib1.dll` next to it (from
MSYS2's `ucrt64/bin`), or run it from the UCRT64 shell. With Visual Studio:
the same `cmake` lines, with SDL2 and zlib from vcpkg
(`-DCMAKE_TOOLCHAIN_FILE=.../vcpkg/scripts/buildsystems/vcpkg.cmake`).

**Cross-compiling** on Linux (gives the single `hipiview.exe`, the same
way the workflow does):

```bash
sudo apt install g++-mingw-w64-x86-64-posix curl zip
tools/hipiview/ci/build-windows.sh          # fetches SDL2 and zlib into build-win/
```

It leaves `tools/hipiview/hipiview-windows.zip`. By hand instead:
`make windows SDL2_MINGW=<SDL2 mingw package>/x86_64-w64-mingw32 ZLIB_MINGW=<folder with
include/zlib.h, include/zconf.h, lib/libz.a>`.

**On Windows, good to know:**

- **The port** is found by HIPI's USB ids (interface 7 of VID 2E8A / PID
  000B) or by its name, "HIPI Display Mirror". Windows 10 and 11 have the
  driver built in: each of HIPI's four serial ports gets a COM number
  (Device Manager → Ports). hipiview turns DTR on itself, as HIPI wants.
- **Recording** needs `ffmpeg.exe` in the PATH (or in the same folder as
  hipiview.exe), e.g. from [gyan.dev](https://www.gyan.dev/ffmpeg/builds/).
- **The picture cache** is in `%LOCALAPPDATA%\hipiview`.
- Screenshots, videos and `--log` files go to the folder hipiview is run
  from.

## Use

```bash
./hipiview                        # finds the mirror port by its name
./hipiview /dev/ttyACM3           # or name it
./hipiview --record demo.mp4      # record from the start
```

| Key | Does |
|-----|------|
| **R** | Start/stop recording (`hipiview-<date>-<time>.mp4`) |
| **S** | Screenshot (`hipiview-<date>-<time>.png`) |
| **F** | Full screen on/off (**Esc** leaves it too) |
| **B** | Backlight dimming on/off (see below) |
| **T** | Touch rings on/off (see below) |
| **Q** | Quit (a recording in progress is saved) |

The window can be resized; the picture keeps its proportions.

### Controlling HIPI from the PC

With **Mirror to PC < Control >** on HIPI, hipiview works HIPI's touch
screen and buttons, so HIPI can be used from the PC as well as on its own
screen:

| On the PC | On HIPI |
|-----------|---------|
| Click | A tap, where you click |
| Drag and let go | A swipe (left/right: another view, up/down: scroll a list) |
| Click and hold | A touch held (e.g. ▲/▼ on the button strip repeat) |
| **Enter** | OK |
| **↑** / **↓** (held: repeated) | ▲ / ▼ |
| Mouse wheel | ▲ / ▼ |
| **Backspace** | X |
| **Tab**, or holding **Shift** with Enter/↑/↓/Backspace | Shift (Shift+Enter = EXIT, Shift+Backspace = CLR, ...) |
| **←** / **→** | The previous / next view |

A key slides the button strip in, so the press can be seen, and acts at
once. Everything works exactly as with a finger on the screen: the first
touch only wakes a sleeping screen, menus, swipes, the Tape view's
buttons. If you touch HIPI's screen and click in hipiview at the same time,
the one that came first wins until it's let go of.

**Touch rings:** where you click, a ring shows for a moment — in the window
and in the video, so whoever watches can see where was touched. They're
also kept in a `--log` file, so `--render` draws them too. **T** turns them
off (and on).

With **View** the mouse and keys do nothing on HIPI (the window keys above
still work): the title bar then says "VIEW ONLY" for a moment when you click
or press a key, and with **Control** it shows "control".

### Options

| Option | Meaning |
|--------|---------|
| `--record FILE` | Record from the start (any name ffmpeg understands, e.g. `.mp4`, `.mkv`) |
| `--fps N` | Video frame rate, default 30 |
| `--crf N` | Video quality (x264 CRF), lower is better, default 18 |
| `--log FILE` | Also save the raw stream, to replay or render later |
| `--replay FILE` | Play a saved stream in the window instead of using the port |
| `--render FILE` | With `--replay`: turn the saved stream into a video, as fast as possible, with no window |
| `--scale X` | Window size factor, e.g. `0.75` or `1.5` |
| `--backlight` | Dim the picture like the real backlight |
| `--test-input FILE` | For tests: play mouse and key events from FILE (see `test/link_sim.cpp`) |

**The cleanest recordings** come from saving the stream and rendering it
afterwards:

```bash
./hipiview --log take1.hvlog                           # show, and save the stream
./hipiview --replay take1.hvlog --render take1.mp4     # then make the video
```

Rendering uses the stream's own timing, so the video never drops or
repeats frames, even if the PC was busy while you were recording.

**Backlight:** by default the picture is always at full brightness, which is
usually what you want in a video. With `--backlight` (or **B**) the picture
is dimmed like the real panel, e.g. when the screen saver dims it.

## The port

(Windows: see [Windows](#windows) above.) On Linux, the mirror port is the
fourth serial port HIPI creates, usually `/dev/ttyACM3`. hipiview finds it by its interface name, so you normally
don't need to give it. If HIPI is restarted or unplugged, hipiview waits and
connects again by itself.

To open the port, your user must be in the `dialout` group:

```bash
sudo usermod -aG dialout $USER     # then log out and in again
```

If **ModemManager** is installed, it may probe new serial ports for a few
seconds after HIPI is plugged in. That's harmless, but you can tell it to
leave HIPI alone:

```bash
echo 'ATTRS{idVendor}=="2e8a", ATTRS{idProduct}=="000b", ENV{ID_MM_DEVICE_IGNORE}="1"' | \
    sudo tee /etc/udev/rules.d/99-hipi.rules
sudo udevadm control --reload-rules
```

## Good to know

- **Lost data.** USB loses a packet now and then on its way to the PC. The
  stream therefore goes in numbered frames with a checksum. hipiview
  acknowledges what it gets and asks for a resend when something is
  missing, and HIPI keeps every frame until it's acknowledged. A lost
  packet only costs a resend, which the title bar counts ("resent: n").
  Only if that doesn't help within two seconds does hipiview ask for the
  whole screen again ("restarts: n").
- **HIPI restarting.** When HIPI goes into **Bootsel mode** (for a firmware
  update), it first lets hipiview get the last picture, and the title bar
  says so. While HIPI is gone, the last picture stays. When HIPI comes back
  (new firmware, a reset, the cable plugged in again), hipiview starts
  afresh from a black screen and reads everything again.
- **hipiview suspended or hung.** If hipiview stops reading but keeps the
  port open (Ctrl+Z in the terminal, say), HIPI notices within a few
  seconds and stops sending, so it doesn't slow down. When hipiview runs
  again, it asks for the screen by itself.
- **Speed.** Connecting takes about as long as reading the panel itself
  (1.2 MB) back from the display chip, plus the pictures the first time. The
  chip can only be read one byte per SPI transfer.
  Text, menus and plots are small and go through easily.
  Big pictures (the Tape view, the screen saver animations) are a lot of
  data. If the USB port can't keep up, HIPI waits for it, so heavy animations
  may run a little slower while hipiview is connected. If hipiview stops
  reading for more than a second, HIPI gives up and starts a fresh session
  (the picture is sent again).
- **The built-in font.** The chip's own font (used in the menus) is read
  from the real chip when hipiview connects, so menu text looks exactly as
  on the panel.
- **The blinking text cursor** is drawn by the chip itself, outside its
  memory. hipiview draws it as a block in the text colour, blinking at
  the same rate. Its exact look on the real panel may differ slightly.
- **5" panel:** not supported. The port exists, but stays silent.

## Testing without HIPI

`make test` runs HIPI's real display driver, its `Screen` class and the
mirror code on the PC against an emulated chip. It checks that a viewer
connecting midway, and then following live drawing, ends up with exactly
the same picture and memory. It also writes `test_stream.hvlog`, which you
can try with `--replay` or `--render`.

`make link_sim && ./link_sim 0.02` runs the whole chain over a "USB" that
loses 2 % of its packets: HIPI's real `display_mirror.cpp`, with TinyUSB
stood in by the test, talks to the real hipiview through a
pseudo-terminal. Afterwards it checks that the picture came out exactly
right. It does up to about 5 % loss. `./link_sim 0.01 20 stall` freezes hipiview
for a while midway and checks that HIPI keeps running smoothly and that
the picture comes back afterwards. `./link_sim 0.01 18 input` has hipiview
click, swipe and press keys (`--test-input`) and checks that HIPI gets them
as touches and buttons with **Control** — and nothing with **View**.

## Files

| File | Contents |
|------|----------|
| `hipiview.cpp` | The program: port, window, recording, replay |
| `deframer.hpp` | The viewer's end of the link: frames, CRC, acknowledgements, resends |
| `lt7683_emu.hpp/.cpp` | The emulated LT7683 |
| `png.hpp` | Screenshots |
| `input.hpp` | Mouse and keys to HIPI; the touch rings |
| `serial_port.hpp/.cpp` | The serial port and finding it: POSIX (Linux, macOS) and Win32 |
| `CMakeLists.txt` | Building with CMake (Windows, Linux, macOS); the `Makefile` does Linux and the tests |
| `ci/build-windows.sh` | The Windows build, as GitHub (and you) run it |
| `test/host_test.cpp` | The test (`make test`) |
| `test/link_sim.cpp`, `test/stubs/` | The lossy-USB test (`make link_sim`) |
| `../../include/mirror_protocol.h` | The stream format, shared with the firmware |
| `../../include/mirror_encoder.hpp`, `mirror_session.hpp` | Firmware side, also used by the test |
| `../../src/display_mirror.cpp` | Firmware: the mirror port |
