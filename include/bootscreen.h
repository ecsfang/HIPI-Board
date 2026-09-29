// bootscreen.h -- the start-up screen (7" panel): logo, version, a STATUS
// card whose rows are filled in one by one as start-up gets there (with a
// short "..." animation and a coloured result dot), the devices on the
// loop, and a "Ready" footer with a countdown bar. Drawn in the menu
// layer and shown as a full-screen PIP overlay, so anything drawn on the
// panel meanwhile (Screen's start-up text) stays hidden underneath.
#pragma once
#include "display_config.h"
#include "hpil.h"
#include <vector>

namespace hipi {

enum class BootState { Ok, Fail, Off, Info };

// Frame, header (logo, name, version) and the two empty cards
void bootscreen_begin(DisplayDriver* display, const char* version);
// Adds the next STATUS row: label, then "..." and the result
void bootscreen_row(const char* label, BootState state, const char* value);
// Fills the DEVICES card: every device in loop order, on/off
void bootscreen_devices(const std::vector<CDevice*>& devices);
// Footer "Ready" + countdown bar, fraction 0..1 of the waiting time gone
void bootscreen_progress(float fraction);
// Removes the screen (the panel underneath shows again)
void bootscreen_end();

}  // namespace hipi
