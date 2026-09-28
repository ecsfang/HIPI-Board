// screendump.h -- saves what the panel shows as a BMP file on the SD card
// ("screendump_<n>.bmp"). The running number is kept in CONFIG.TXT
// (screendump_next); numbers whose file already exists are skipped.
#pragma once
#include "display_config.h"
#include <string>

namespace hipi {

// Reads back the live panel (the main window only, unless withOverlays --
// on the 7" panel the menu, button strip and view-switch box are PIP
// overlays and never part of it) and writes it as a 24-bit BMP. Takes a few seconds; HP-IL isn't
// serviced meanwhile. Returns true on success; `message` is then the file
// name, otherwise a short error text for the user.
// 7" panel only for now -- the RA8875 (5") has no read-back implemented.
// withOverlays: also include what's shown in the PIP windows -- menu,
// info box, device list, message box (PIP-1) and the button strip (PIP-2)
// -- composited in software, i.e. exactly what's on the screen. Mostly
// for documentation/development.
bool screendump_save(DisplayDriver* display, std::string& message, bool withOverlays = false);

// Deferred request, e.g. from an HP-IL escape sequence (Screen's
// "ESC # D" / "ESC # M", see pico_main.cpp): taking a dump takes seconds,
// so it must not run inside HP-IL frame processing. boardui_poll() picks
// it up with screendump_takePending() and runs it from the main loop.
void screendump_request(bool withOverlays);
bool screendump_takePending(bool& withOverlays);

}  // namespace hipi
