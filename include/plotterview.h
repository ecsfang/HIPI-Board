#pragma once
// plotterview.h -- the device views on the panel: the normal HP-41 text
// display (Screen), a live rendering of the plotter's output (CPlotter),
// and (7" panel only) one Tape view per cassette drive: a picture of an
// HP82161A with touch-sensitive areas. Each device says which view it has
// (CDevice::viewKind()); swiping steps through them in loop order. See
// boardui.h for the button strip/info-box/status-LED "chrome" this
// coordinates with.
//
// Usage (see pico_main.cpp):
//
//   plotter = new CPlotter("TFPLOT");   // (done inside hipi_init())
//   hipi::plotterview_init(display, screen, plotter);
//
// The UiDialog "Display" menu calls plotterview_viewDevices()/
// plotterview_showDevice()/plotterview_clearPlotter() directly (see
// uidialog.hpp) -- there's no separate callback wiring needed here.

#include "display_config.h"
#include "Screen.hpp"
#include "plotter.h"
#include <cstdint>
#include <string>
#include <vector>

class CDrive;
class CDevice;

namespace hipi {

// The kinds of view the panel can show (matches ViewKind in hpil.h, plus
// the Analyzer, which isn't a device's view)
enum class DisplayOutput { Display, Plotter, Tape, Analyzer, Clock, LoopMap, Signals, CharTable };

// Touch-sensitive areas of the Tape view -- see plotterview_tapeHitTest().
enum class TapeHotspot { None, Open, Power, Rewind };

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
// Shows a short message (e.g. "Saved screendump_3.bmp") in the same
// yellow box as the view-switch splash, for 2.5 s. Taps are ignored
// meanwhile, like during the splash.
void plotterview_showMessage(const char* text);

void plotterview_setDrive(CDrive* drive);
// The drive given to plotterview_setDrive() (nullptr if none) -- e.g. for
// the power switch hotspot, which toggles it (see boardui_handleTap()).
CDrive* plotterview_drive();

// True if the view can be shown on this build/board. Tape is only
// available on the 7" panel, and only if hp82161a.bmp loaded at boot.
bool plotterview_isAvailable(DisplayOutput mode);

// The kind of view currently shown
DisplayOutput plotterview_output();

// Swipe (see boardui.cpp's boardui_handleSwipe()): shows the view of the next (forward) or previous device on the
// loop that has one (CDevice::viewKind()), in loop order.
void plotterview_cycleOutput(bool forward);

// The devices that currently have a view (switched on, view available
// here), in loop order -- e.g. for the Display menu
std::vector<CDevice*> plotterview_viewDevices();
// A view's name: "Display", "Plotter", "Tape" -- plus the device name if
// several devices have that kind of view ("Tape TFDRIVE2")
std::string plotterview_viewTitle(CDevice* dev);
// Shows that device's view
void plotterview_showDevice(CDevice* dev);
// The device whose view is showing (nullptr if none)
CDevice* plotterview_viewDevice();

// The HP-IL analyzer's view (analyzer.h) -- not a device, so not in the
// swipe order. Leaving it returns to the view shown before.
void plotterview_showAnalyzer();
void plotterview_leaveAnalyzer();

// The HP-IL loop map (loopmap.h) -- not a device, not in the swipe order;
// leaving it returns to the view shown before
void plotterview_showLoopMap();
void plotterview_leaveLoopMap();

// HP-IL signal diagnostics (hpil_diag.h) -- like the loop map
void plotterview_showSignals();
void plotterview_leaveSignals();

// System -> Character table (chartable.h) -- a help page, like the loop map
void plotterview_showCharTable();
void plotterview_leaveCharTable();

// The clock screen saver (clock.h) -- shown and left without the splash
void plotterview_showClock();
void plotterview_leaveClock();

// Call once per main-loop iteration -- handles the view-switch splash's
// auto-dismiss timer (see plotterview_showDevice()).
void plotterview_poll();

// True while the brief "DISPLAY"/"PLOTTER"/"TAPE" switch-announcement
// splash is showing (see plotterview_showDevice()). Checked by
// UiDialog::close() (7": don't hide the PIP overlay the splash now owns;
// 5": don't redraw over the full-screen splash) and by
// boardui_handleTap() (7": taps are ignored while it's up).
bool plotterview_isSplashVisible();

// Clears the plotter's drawing (both the retained segments() history and,
// if Plotter output is currently showing, the on-screen drawing too) --
// the "new paper" menu action. Safe to call regardless of which output
// mode is currently showing.
void plotterview_clearPlotter();

// Plot view: true = Fit (the plot itself fills the screen), false = Page
// (the whole P1..P2 area, like the paper). Display -> Plot view.
void plotterview_setPlotFit(bool fit);
bool plotterview_plotFit();

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
