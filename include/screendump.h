// screendump.h -- saves what the panel shows as a BMP file on the SD card
// ("screendump_<n>.bmp"). The running number is kept in CONFIG.TXT
// (screendump_next); numbers whose file already exists are skipped.
#pragma once
#include "display_config.h"
#include <string>

// Development aid: set to 1 to also save screen dumps (with menus) of the
// screens that can't be dumped the normal way -- the "Entering Bootsel
// mode" message (the board reboots right after), and the "Connected to
// PC" message + the Config menu showing "Disconnect from PC" (the SD card
// belongs to the PC then), and the start-up screen twice: halfway
// through its STATUS rows, and finished with the countdown bar halfway. Keep 0 for normal builds. Can also be set from
// the build: -DHIPI_DEV_SCREENDUMPS=1
#ifndef HIPI_DEV_SCREENDUMPS
#define HIPI_DEV_SCREENDUMPS 0
#endif

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
