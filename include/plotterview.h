#pragma once
// plotterview.h -- switches the panel's full-screen output between the
// normal HP-41 text display (Screen), a live rendering of the plotter's
// output (CPlotter), and (7" panel only) the Tape view: a picture of an
// HP82161A cassette drive with touch-sensitive areas. See boardui.h for the button
// strip/info-box/status-LED "chrome" this coordinates with.
//
// Usage (see pico_main.cpp):
//
//   plotter = new CPlotter("TFPLOT");   // (done inside hipi_init())
//   hipi::plotterview_init(display, screen, plotter);
//
// The UiDialog "Display" menu calls plotterview_setOutput()/
// plotterview_clearPlotter()/plotterview_output() directly (see
// uidialog.hpp) -- there's no separate callback wiring needed here.

#include "display_config.h"
#include "Screen.hpp"
#include "plotter.h"
#include <cstdint>
#include <string>

class CDrive;

namespace hipi {

enum class DisplayOutput { Display, Plotter, Tape };
// Bump this alongside the enum whenever a new view is added (e.g. a future
// Printer view) -- plotterview_cycleOutput() cycles through 0..count-1
// generically, skipping any view that isn't available (see
// plotterview_isAvailable()).
constexpr int kDisplayOutputCount = 3;

// Touch-sensitive areas of the Tape view -- see plotterview_tapeHitTest().
enum class TapeHotspot { None, Open, Power };

// Call once, after display/screen/plotter all exist (see hipi_init()).
// On the 7" panel this also loads hp82161a.bmp from the SD card into its own
// off-screen SDRAM layer (LT7683::kTapeLayerAddr) for the Tape view.
void plotterview_init(DisplayDriver* display, Screen* screen, CPlotter* plotter);

// Tape view cassette (7" only; needs tape-in.bmp on the SD card): the
// cassette shows the selected .dat file's name on its label. Call
// plotterview_setTapeFile() at boot and whenever another file is
// selected ("" = no file, empty drive). plotterview_setTapeEjected(true)
// shows the drive empty with its lid open (open.bmp, if present)
// while the file picker is open. Both may be called
// from inside menu drawing -- they restore the active canvas themselves.
void plotterview_setTapeFile(const std::string& filename);
void plotterview_setTapeEjected(bool ejected);

// Tape view status LEDs (7" only; needs leds.bmp): POWER is lit while the
// drive device is enabled, BUSY while it's accessing its file (see
// CDrive::isBusy()). Updated from plotterview_poll().
void plotterview_setDrive(CDrive* drive);
// The drive given to plotterview_setDrive() (nullptr if none) -- e.g. for
// the power switch hotspot, which toggles it (see boardui_handleTap()).
CDrive* plotterview_drive();

// True if the view can be shown on this build/board. Tape is only
// available on the 7" panel, and only if hp82161a.bmp loaded at boot.
bool plotterview_isAvailable(DisplayOutput mode);

// Switches the panel's full-screen output. Handles suspending/resuming
// Screen and hiding/showing the button strip (Plotter mode uses the full
// 800x480 panel, so the strip is suppressed while it's active), and
// (re)drawing whichever output is now current.
void plotterview_setOutput(DisplayOutput mode);
DisplayOutput plotterview_output();

// Cycles to the next (forward=true) or previous (forward=false) view in
// the list, wrapping around at either end -- used for the left/right
// swipe gesture (see boardui.cpp's boardui_handleSwipe()). Generic over
// kDisplayOutputCount, so it already works correctly for however many
// views exist whenever that count grows.
void plotterview_cycleOutput(bool forward);

// Call once per main-loop iteration -- handles the view-switch splash's
// auto-dismiss timer (see plotterview_setOutput()).
void plotterview_poll();

// True while the brief "DISPLAY"/"PLOTTER"/"TAPE" switch-announcement
// splash is showing (see plotterview_setOutput()). Checked by
// UiDialog::close() (7": don't hide the PIP overlay the splash now owns;
// 5": don't redraw over the full-screen splash) and by
// boardui_handleTap() (7": taps are ignored while it's up).
bool plotterview_isSplashVisible();

// Clears the plotter's drawing (both the retained segments() history and,
// if Plotter output is currently showing, the on-screen drawing too) --
// the "new paper" menu action. Safe to call regardless of which output
// mode is currently showing.
void plotterview_clearPlotter();

// Redraws the current full-screen view from scratch: the plotter output
// (segments() replayed in full) or the Tape image. Called internally when
// switching views or when the menu closes back into one (to erase the
// menu box drawn on top), but exposed in case something else needs to
// force a refresh. Does nothing while Display (text) output is showing.
void plotterview_redraw();

// Redraws only a rectangular sub-region (screen pixel coordinates) of the
// current full-screen view. For Tape this is a single BTE copy from the
// off-screen layer. For Plotter it narrows the active window to that region
// first, so replaying every segment only actually draws the ones (or
// parts of ones) that fall inside it, then restores the full-screen
// active window Plotter mode normally runs with. Used by boardui.cpp's
// hideButtonStrip() to restore just the area the button bitmap occupied,
// instead of a full-screen redraw for what's normally a small strip.
void plotterview_redrawRegion(std::int16_t x0, std::int16_t y0,
                              std::int16_t w, std::int16_t h);

// True while a non-text full-screen view (Plotter or Tape) is showing,
// i.e. Screen is suspended and anything drawn over the view (such as the
// button strip) must be restored via plotterview_redrawRegion().
bool plotterview_isActive();

// Returns which Tape hotspot (if any) contains the given screen point.
// Always TapeHotspot::None unless the Tape view is showing and its
// switch splash has timed out.
TapeHotspot plotterview_tapeHitTest(std::uint16_t x, std::uint16_t y);

}  // namespace hipi
