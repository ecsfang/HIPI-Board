# hipiview — HIPI's display on the PC

`hipiview` shows HIPI's 7" display in a window on the PC, live, and
records it straight to a video file. It's useful for making videos: no
camera, no reflections, no lighting to get right. Every pixel is exactly
what the panel shows, at the panel's own 1024 × 600.

It works with the **7" panel** (LT7683) only.

## How it works

First switch the mirror on in HIPI: **Settings → Screen → Mirror to PC
< On >** (saved; off from the start). With it off, HIPI ignores hipiview
and the title bar says "no answer from HIPI".

HIPI has a fourth USB serial port, **HIPI Display Mirror** (CDC 3). Once
hipiview has opened it and asked for the screen, HIPI sends everything it
sends to the display controller to that port as well. hipiview runs an emulated LT7683 on that
stream: SDRAM layers, text engine, drawing engine, block transfers, PIP
windows (the menu and the button strip), the blinking cursor, and so on.
The picture is therefore built exactly the way the real chip builds it, and
anything added to the firmware later is mirrored too.

When hipiview connects, HIPI first sends its current state: the register
values, the chip's built-in font, and the contents of the panel and the
off-screen layers. What is visible comes first (the panel, an open menu, the
button strip), then the rest. The title bar shows the progress, and HIPI
keeps working normally meanwhile. When hipiview closes the port, mirroring
stops and HIPI no longer spends any time on it.

**Picture cache.** The Tape view's pictures (from HIPI's SD card) are sent
only the first time. hipiview keeps them in `~/.cache/hipiview/`, and later
sessions only name them. If you replace a picture on the SD card, its new
version is sent automatically, because a picture's id comes from its file
name, size and date. Every cached picture carries a checksum: a damaged
one (or one from an older hipiview) is thrown away and simply sent again.
You can delete the folder at any time; it just fills up again.

## Building (Ubuntu)

```bash
sudo apt install build-essential libsdl2-dev zlib1g-dev ffmpeg
cd tools/hipiview
make
```

`ffmpeg` is only needed for recording.

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
| **F** | Full screen on/off |
| **B** | Backlight dimming on/off (see below) |
| **Q**, **Esc** | Quit (a recording in progress is saved) |

The window can be resized; the picture keeps its proportions.

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

The mirror port is the fourth serial port HIPI creates, usually
`/dev/ttyACM3`. hipiview finds it by its interface name, so you normally
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
the picture comes back afterwards.

## Files

| File | Contents |
|------|----------|
| `hipiview.cpp` | The program: port, window, recording, replay |
| `deframer.hpp` | The viewer's end of the link: frames, CRC, acknowledgements, resends |
| `lt7683_emu.hpp/.cpp` | The emulated LT7683 |
| `png.hpp` | Screenshots |
| `test/host_test.cpp` | The test (`make test`) |
| `test/link_sim.cpp`, `test/stubs/` | The lossy-USB test (`make link_sim`) |
| `../../include/mirror_protocol.h` | The stream format, shared with the firmware |
| `../../include/mirror_encoder.hpp`, `mirror_session.hpp` | Firmware side, also used by the test |
| `../../src/display_mirror.cpp` | Firmware: the mirror port |
