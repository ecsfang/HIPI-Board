#pragma once
// boardui.h -- board "chrome": splash screen, the auto-hiding button strip,
// the top-left info box, and the USB/PILBOX status LEDs. Everything here is
// passive/informational UI, as opposed to the interactive settings menu
// implemented by UiDialog (see uidialog.hpp).
//
// Usage (see pico_main.cpp):
//
//   display->begin();
//   hipi::showSplashScreen(display, version, 2000);
//   ...
//   const std::uint16_t stripWidth = hipi::boardui_loadButtonStrip(display);
//   screen = new hipi::Screen(display, ..., SCREEN_MAX_X - stripWidth);
//   dialog = new hipi::UiDialog(display, *screen);
//   hipi::boardui_init(screen, dialog, version);
//
//   touch_set_tap_callback(hipi::boardui_handleTap);
//   touch_set_release_callback(hipi::boardui_handleRelease);
//   touch_set_swipe_callback(hipi::boardui_handleSwipe);
//   touch_set_vertical_swipe_callback(hipi::boardui_handleVerticalSwipe);
//
//   while (running) {
//       touch_poll();
//       hipi::boardui_poll();
//   }

#include <cstdint>
#include "display_config.h"
#include "Screen.hpp"
#include "uidialog.hpp"

namespace hipi {

// Splash screen shown as early as possible at boot, right after
// display->begin(), before Screen/UiDialog even exist. Blocks for
// durationMs.
void showSplashScreen(DisplayDriver* display, const char* version,
                      std::uint32_t durationMs = 2000);

// Call once, right after DisplayDriver::begin() and before constructing
// Screen -- draws and caches the button-strip bitmap, and returns its
// pixel width so the caller can size Screen's initial text width
// (SCREEN_MAX_X - returned width). Returns 0 if the bitmap couldn't be
// loaded (e.g. missing from the SD card).
std::uint16_t boardui_loadButtonStrip(DisplayDriver* display, const char* bmpPath = "buttons.bmp");

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
// normal appearance once the finger lifts. Both work the same regardless
// of which output plotterview.h is currently showing -- the button strip
// always shows/hides/responds normally; only *what's underneath it* (text
// vs. the plotter's drawing) differs, see hideButtonStrip() in boardui.cpp.
void boardui_handleTap(std::uint16_t x, std::uint16_t y);
void boardui_handleRelease();

// Register with touch_set_swipe_callback() (see touch.h). Cycles the
// current display output (see plotterview.h) forward/backward -- ignored
// while the menu is open, so a swipe during menu navigation doesn't also
// switch views underneath it.
void boardui_handleSwipe(bool forward);

// Register with touch_set_vertical_swipe_callback() (see touch.h). Scrolls
// the "Devices" list dialog (bottom-left corner tap) when it's open --
// does nothing otherwise.
void boardui_handleVerticalSwipe(bool down);

// Called by UiDialog::close() (see uidialog.hpp) -- shortens the button
// strip's auto-hide countdown to a quick ~0.5s instead of leaving
// whatever's left of the normal 5s inactivity window, since explicitly
// closing the menu (Back/Exit) already signals "done", unlike just going
// quiet mid-navigation.
void boardui_onMenuClosed();

}  // namespace hipi