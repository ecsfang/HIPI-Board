# hp82163_cpp — HP82163-emulator på RA8875, C++17 för Raspberry Pico 2

Port av J. Chillans MicroPython-klass `Screen` (HP82163 / HP-41
videodisplay-emulator) till C++17, plus port av RA8875-drivrutinen.

## Snabbstart i VS Code (Raspberry Pi Pico extension)

### 1. Installera pluginen
Extensions-panelen → sök **"Raspberry Pi Pico"** → installera.

### 2. pico-sdk
Om du inte redan har det via pico-setup:
```bash
git clone -b master https://github.com/raspberrypi/pico-sdk.git ~/pico-sdk
cd ~/pico-sdk && git submodule update --init
```
Pluginen hittar det via `PICO_SDK_PATH`.  Om din SDK ligger under
`~/.pico-sdk/sdk/2.2.0/` (standard efter pico-setup), exportera:
```bash
export PICO_SDK_PATH=$HOME/.pico-sdk/sdk/2.2.0
```

### 3. Importera projektet
Öppna **inte** mappen via `File → Open Folder`.  Kör istället:

**Kommandopalett** (`Ctrl+Shift+P`) →
```
Raspberry Pi Pico: Import Project
```

Peka ut mappen som innehåller `CMakeLists.txt` (dvs `hp82163_cpp/`),
välj board **pico2** (Pico 2 / RP2350).

### 4. Bygg
- **Build**-knappen i statusbaren längst ner, eller `Ctrl+Shift+B`
- Första bygget tar ~1–2 min
- `.uf2`-filen hamnar i `build/hp82163_pico_demo.uf2`

### 5. Flasha
Håll **BOOTSEL** på Pico 2, släpp efter reset, drag-and-drop UF2 till
den nya enheten.

---

## Projektstruktur
```
hp82163_cpp/
├── CMakeLists.txt             ← top-level build
├── project.pico               ← GENERERAS av pluginen (import-steget)
├── README.md
├── .vscode/
│   ├── settings.json
│   └── tasks.json
├── include/  (6 headers)
└── src/     (6 .cpp + 2 demo)
```

Inga lokala `pico_sdk_import.cmake` eller liknande — pluginen sätter
upp SDK via sin toolchain-file och `CMakeLists.txt` inkluderar bara
`${PICO_SDK_PATH}/pico_sdk_init.cmake`.

## Pinout på Pico 2 (samma som din `share.py`)
| Funktion     | GPIO     |
|--------------|----------|
| SPI0 SCK     | GP2      |
| SPI0 MOSI    | GP3      |
| SPI0 MISO    | GP0      |
| CS (active L)| GP1      |
| RST (active L)| GP4     |

## Bygga från kommandoraden
```bash
export PICO_SDK_PATH=$HOME/.pico-sdk/sdk/2.2.0   # eller ~/pico-sdk
cmake -G Ninja -B build -S .
cmake --build build
picotool load -f build/hp82163_pico_demo.uf2
```

## Användning i din egen kod
```cpp
#include "PicoSpiTransport.hpp"
#include "RA8875.hpp"
#include "Screen.hpp"

hp82163::PicoSpiTransport t(spi0, 6'000'000, /*cs=*/1, /*rst=*/4);
hp82163::RA8875 display(t, 800, 480);
display.begin();
display.set8Bpp();           // spegla share.py
display.set2LayerConfig();   // spegla share.py

hp82163::Screen screen(display, 0xFF, /*size=*/0, /*brightness=*/200);
for (uint8_t b : hp41_stream) screen.pr_char(b);
```

## Skillnader mot MicroPython
- Global state i `share.py` → konstruktor-argument
- `RA8875Transport` = virtuellt SPI/GPIO-interface (byt plattform lätt)
- `delayMs()` ersätter `time.sleep()`
- `_write_reg` / `_write_reg16` → publikt `writeReg` / `writeReg16`

## Referenser
- MicroPython `share.py` + `RA8875.py`: J. Chilla, mars 2026
- HP82163-protokollet: HP82163A videodisplay för HP-41
- RA8875: <https://github.com/adafruit/Adafruit_CircuitPython_RA8875>
- pico-sdk: <https://github.com/raspberrypi/pico-sdk>
- Pico VS Code extension: <https://github.com/raspberrypi/pico-vscode>
