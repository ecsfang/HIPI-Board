#pragma once
// boardui.h -- board "chrome": splash screen, the auto-hiding button strip,
// the top-left info box, and the USB/PILBOX status LEDs. Everything here is
// passive/informational UI, as opposed to the interactive settings menu
// implemented by UiDialog (see uidialog.hpp).
//
// Usage (see pico_main.cpp):
//
//   display->begin();
//   hp82163::showSplashScreen(display, version, 2000);
//   ...
//   const std::uint16_t stripWidth = hp82163::boardui_loadButtonStrip(display);
//   screen = new hp82163::Screen(display, ..., SCREEN_MAX_X - stripWidth);
//   dialog = new hp82163::UiDialog(display, *screen);
//   hp82163::boardui_init(screen, dialog, version);
//
//   touch_set_tap_callback(hp82163::boardui_handleTap);
//   touch_set_release_callback(hp82163::boardui_handleRelease);
//
//   while (running) {
//       touch_poll();
//       hp82163::boardui_poll();
//   }

#include <cstdint>
#include "RA8875.hpp"
#include "Screen.hpp"
#include "uidialog.hpp"

namespace hp82163 {

// Splash screen shown as early as possible at boot, right after
// display->begin(), before Screen/UiDialog even exist. Blocks for
// durationMs.
void showSplashScreen(RA8875* display, const char* version,
                      std::uint32_t durationMs = 2000);

// Call once, right after RA8875::begin() and before constructing Screen --
// draws and caches the button-strip bitmap, and returns its pixel width so
// the caller can size Screen's initial text width
// (SCREEN_MAX_X - returned width). Returns 0 if the bitmap couldn't be
// loaded (e.g. missing from the SD card).
std::uint16_t boardui_loadButtonStrip(RA8875* display, const char* bmpPath = "buttons.bmp");

// Call once, after Screen and UiDialog exist -- wires up the module state
// needed by the rest of boardui's functions (info box, button strip,
// status LEDs). `version` is used in the info box's title line.
void boardui_init(Screen* screen, UiDialog* dialog, const char* version);

// Call once per main-loop iteration -- handles the button strip's and info
// box's auto-hide timers, and the periodic USB/PILBOX status LED poll.
void boardui_poll();

// Register these with touch_set_tap_callback()/touch_set_release_callback()
// (see touch.h). boardui_handleTap() decides whether a confirmed touch
// dismisses/opens the info box, wakes/uses the button strip, or forwards to
// the UiDialog menu; boardui_handleRelease() restores a pressed button's
// normal appearance once the finger lifts.
void boardui_handleTap(std::uint16_t x, std::uint16_t y);
void boardui_handleRelease();

}  // namespace hp82163
