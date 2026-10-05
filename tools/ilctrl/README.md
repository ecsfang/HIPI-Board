# ilctrl — HP-IL controller on the PC (PIL-Box CON mode)

A small command-line HP-IL loop controller for Linux. It talks to a PIL-Box —
or HIPI's PILBOX — over a serial port, puts it in **CON** mode (the PC is the
loop's controller) and runs HP-IL transactions on the loop. A C++ port of the
ideas in J-F Garnier's *ILCtrl 1.05* (Visual Basic, GPL): same PIL-Box
protocol, same transactions, a text interface instead of a window.

## Build

```bash
cd tools/ilctrl
make            # or: g++ -std=c++17 -O2 -Wall -o ilctrl ilctrl.cpp
```

## Run

```bash
./ilctrl /dev/ttyACM1                          # interactive
./ilctrl /dev/ttyACM1 -c "scan; did 2; spoll 2"  # commands from the command line
```

`/dev/ttyACM1` is HIPI's PILBox port (the second of its two serial ports).
At start ilctrl sends TDIS, then CON; at the end TDIS again.

| Command | Does |
|---------|------|
| `scan` | Auto-address the loop, then device ID and accessory ID of every device |
| `addr <n>` | Current device address (1–30) for the commands below |
| `did [addr]` | Device ID (SDI) |
| `aid [addr]` | Accessory ID (SAI) |
| `spoll [addr]` | Serial poll: status bytes (SST) |
| `enter [addr]` | Read a line of text from the device (SDA) |
| `send <addr> <text>` | Send text (plus CR LF) to the device |
| `trigger [addr]` | Device trigger (GET) |
| `remote` / `local` | REN / NRE to all devices |
| `clear` | DCL: clear all devices |
| `frame <hex>` | Send one raw frame (e.g. `frame 490` = IFC) and show what comes back |
| `scope off\|out\|in\|both` | Show the frames sent / received |
| `con` / `coff` / `tdis` | PIL-Box mode commands |
| `help`, `quit` | |

## Testing HIPI's CON mode

1. On HIPI: **Settings → CON loop → Internal** — the loop is closed inside
   HIPI, so no cable is needed and the PC reaches HIPI's own devices.
   (With **Cable**, a cable must go from HIPI's HP-IL OUT back to its IN,
   possibly through other devices; no HP-41 on that loop.)
2. Close pyILPER (only one program can use the port).
3. `./ilctrl /dev/ttyACM1` — HIPI's start-up screen / device list now shows
   PILBOX in **CON**.
4. `scan` lists HIPI's devices in loop order: the ones after PILBOX in
   **Devices → Change order** come first, then (with a cable) the cable's
   devices, then the ones before PILBOX.
5. Try a device: e.g. `send 1 HELLO` (the display, if it's device 1),
   `did 2`, `spoll 2` (the cassette drive's status), `scope both` to watch
   the frames.

`TRANSMIT ERR` means a frame didn't come back in time (HIPI also shows a
warning box then); `NO RESPONSE` that no device answered at that address.

## Protocol notes

Each frame goes to the box as two bytes (8-bit format: high `001c ccd0`, low
`1ddd dddd`); the high byte is left out when it's the same as the last one.
That "last high byte" is **one** value shared by both directions — the box's
answers update it too — exactly as the PIC firmware (`LASTH`) and HIPI
(`PIL_tx_hi`) keep it. Every frame sent comes back once it has gone round
the loop; a command comes back only after the box has sent an RFC round
(the CON handshake).

GPL v2 or later, like ILCtrl.
