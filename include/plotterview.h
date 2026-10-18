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
class CPrinter;

namespace hipi { class Video80; }

namespace hipi {

// The kinds of view the panel can show (matches ViewKind in hpil.h, plus
// the Analyzer, which isn't a device's view)
// (Video80: the Display view while TFDISPLAY emulates the HP 92198)
enum class DisplayOutput { Display, Plotter, Tape, Analyzer, Clock, LoopMap, Signals, CharTable, Printer, Video80, HwTest,
                           MediaMap };

// Touch-sensitive areas of the Tape view -- see plotterview_tapeHitTest().
enum class TapeHotspot { None, Open, Power, Rewind };

// Touch-sensitive areas of the Printer view -- see
// plotterview_printerHitTest(). `value` is the switch position the touch
// asks for (Mode: 0 MAN, 1 TRACE, 2 NORM; Intensity: 0 LIGHT .. 4 DARK).
enum class PrinterHotspot { None, Power, Mode, Intensity, Print, Advance };
struct PrinterHit {
    PrinterHotspot spot = PrinterHotspot::None;
    int value = 0;
};

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

// Printer view (7" only; needs hp82162a.bmp, and prtparts.bmp for the
// switches, POWER light and pressed keys): the printer shown. Its front
// panel follows the device by itself (plotterview_poll()).
void plotterview_setPrinter(CPrinter* printer);
CPrinter* plotterview_printer();
// PRINT / PAPER ADVANCE from the view's keys: passed on to the printer,
// and the key is shown pressed (at least briefly, even for a quick tap)
void plotterview_printerPrint(bool down);
void plotterview_printerAdvance(bool down);

// TFDISPLAY's HP 92198 screen (video80.h). It draws itself; the view
// only switches it on and off.
void plotterview_setVideo80(Video80* v);
Video80* plotterview_video80();
// Puts the blinking text cursor back where the view showing has it
// (Screen's, or the HP 92198 view's) -- after something else has drawn
// text with the chip's one shared text cursor (menu, button strip, boxes)
void plotterview_refreshCursor();
// The display's model (display.h): true = HP 92198, the Display view is
// then Video80 (output DisplayOutput::Video80) instead of Screen. If the
// display is showing, the other one takes over at once.
void plotterview_setDisplayModel(bool v80);

// ── The demo (demo.h) ───────────────────────────────────────────────────
// Shows a view without the name box (dev: the device, for a device's view)
void plotterview_showView(DisplayOutput mode, CDevice* dev);
// The Tape view as if the drive were working: POWER and BUSY lit, the
// reels turning (nothing happens to the drive itself)
void plotterview_setDemoTape(bool busy);
// The printer's paper torn off (CPrinter::clearPaper()), redrawn empty
void plotterview_clearPaper();
// The Printer view's paper, moved by hand (7"): a finger on the paper
// drags it up and down (boardui.cpp, touch_set_drag_callback()). Down
// shows older lines, back to the first one at the top; up pulls the
// newest line out above the tear-off bar, no further. Where it's let go
// it stays -- until something is printed (or PAPER ADVANCE, PRINT, X):
// then it glides back to the print head first.
bool plotterview_paperDragBegin(int x, int y);   // true: on the paper, the touch moves it
void plotterview_paperDragMove(int x, int y);
void plotterview_paperDragEnd();
// Tearing off: the paper dragged up to the tear-off edge and then
// sideways (at least 100 px, finger still down). After the drag ends:
// true once, with the direction (-1 left, 1 right) -- the caller asks
// (Save / Discard / Cancel) and then calls plotterview_tearOff().
bool plotterview_takeTearGesture(int& dir);
// The paper above the tear-off edge slides away to that side, then the
// printer's paper is empty (CPrinter::clearPaper())
void plotterview_tearOff(int dir);
int  plotterview_paperLines();                   // lines on the paper (kept)
// The printer's paper was replaced (a printout loaded): drawn afresh,
// back at the print head
void plotterview_reloadPaper();
void plotterview_paperScrollBy(int lines);       // > 0: older lines (▲), < 0: newer (▼)
int  plotterview_paperWindowLines();             // lines the window shows (Shift+▲/▼)
bool plotterview_paperScrolled();                // not where it prints
void plotterview_paperToLive();                  // back there (glides)
// The drive given to plotterview_setDrive() (nullptr if none) -- e.g. for
// the power switch hotspot, which toggles it (see boardui_handleTap()).
CDrive* plotterview_drive();

// True if the view can be shown on this build/board. Tape is only
// available on the 7" panel, and only if hp82161a.bmp loaded at boot;
// the same for Printer and hp82162a.bmp.
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

// The view chosen from the HP-41 with "ESC # V <addr>" (sent to the
// display device): shows the view of the HIPI device at HP-IL address
// `addr`. requestViewByAddress() only records it (called from HP-IL frame
// handling); boardui_poll() picks it up with takeViewRequest() and runs
// showAddress() from the main loop. If there's no HIPI device at that
// address, or it has no view (or is disabled), nothing happens apart
// from a warning in the log. Returns true if the view was shown.
void plotterview_requestViewByAddress(std::uint8_t addr);
bool plotterview_takeViewRequest(std::uint8_t& addr);
bool plotterview_showAddress(std::uint8_t addr);

// The HP-IL analyzer's view (analyzer.h) -- not a device, so not in the
// swipe order. Leaving it returns to the view shown before.
void plotterview_showAnalyzer();
void plotterview_leaveAnalyzer();

// The HP-IL loop map (loopmap.h) -- not a device, not in the swipe order;
// leaving it returns to the view shown before
void plotterview_showLoopMap();
void plotterview_leaveLoopMap();
// From the loop map's details box: a device's view, if it has one here
// (plotterview_viewName(): "PLOTTER", "TAPE" ..., nullptr if none). X
// there brings back the loop map (plotterview_backToLoopMap() -- true if
// it did); any other way of leaving the view forgets that.
const char* plotterview_viewName(CDevice* dev);
void plotterview_showDeviceFromLoopMap(CDevice* dev);
bool plotterview_cameFromLoopMap();
bool plotterview_backToLoopMap();

// HP-IL signal diagnostics (hpil_diag.h) -- like the loop map
void plotterview_showSignals();
void plotterview_leaveSignals();

// System -> Character table (chartable.h) -- a help page, like the loop map
// System -> Hardware test (hwtest.h) -- not a device's view; leaving it
// goes back to the view before
void plotterview_showHwTest();
void plotterview_leaveHwTest();
// Media map (mediamap.h): the drive's cassette/disk as rings; leaving goes
// back to the view before
void plotterview_showMediaMap();
void plotterview_leaveMediaMap();
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

// Paper colour: false = white paper with dark pens (default), true =
// black paper with light pens (inverted). Plotter -> Paper.
void plotterview_setPaperBlack(bool black);
bool plotterview_paperBlack();

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

// Plotter digitizing (HP-GL DP): plotterview_poll() brings up the Plotter
// view with a panel on the right -- a loupe, X/Y, arrows, ENTER, X. A touch
// on the plot puts the marker there, the arrows move it a pixel (held:
// repeated), ENTER gives the point to the plotter, X cancels.
bool plotterview_digitizing();               // the panel is showing
bool plotterview_digitizeTap(int x, int y);  // true: the touch was taken
void plotterview_digitizeRelease();          // finger up: an arrow stops repeating
// A strip button while digitizing (code as boardui: 2 OK = ENTER, 3/4 Up/
// Down, 5 X = cancel; repeat: held) -- true if taken
bool plotterview_digitizeButton(int code, bool repeat);
// For tests: the centre of a panel control (0 up, 1 down, 2 left, 3 right,
// 4 ENTER, 5 X) and the marker, in screen pixels
bool plotterview_digitizeControl(int id, int& x, int& y);
void plotterview_digitizeMarker(int& x, int& y);

// Returns which Tape hotspot (if any) contains the given screen point.
// Always TapeHotspot::None unless the Tape view is showing and its
// switch splash has timed out.
TapeHotspot plotterview_tapeHitTest(std::uint16_t x, std::uint16_t y);

// Which Printer control (if any) is at the given screen point -- always
// PrinterHotspot::None unless the Printer view is showing and its switch
// splash has timed out.
PrinterHit plotterview_printerHitTest(std::uint16_t x, std::uint16_t y);

}  // namespace hipi
