#pragma once
// views.h -- the device views on the panel: the normal HP-41 text
// display (Screen), a live rendering of the plotter's HP-GL (hpgl.h),
// and (7" panel only) one Tape view per cassette drive: a picture of an
// HP82161A with touch-sensitive areas. Each device says which view it has
// (CDevice::viewKind()); swiping steps through them in loop order. See
// boardui.h for the button strip/info-box/status-LED "chrome" this
// coordinates with. The Plotter view is drawn in plotterview.cpp, the rest
// in views.cpp (what they share: views_impl.h).
//
// Usage (see pico_main.cpp):
//
//   plotter = new CPlotter("TFPLOT");   // (done inside hipi_init())
//   hipi::views_init(display, screen, plotter, &plotter->hpgl());
//
// The UiDialog "Display" menu calls views_viewDevices()/
// views_showDevice()/views_clearPlotter() directly (see
// uidialog.hpp) -- there's no separate callback wiring needed here.

#include "display_config.h"
#include "Screen.hpp"
#include "hpgl.h"
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

// Touch-sensitive areas of the Tape view -- see views_tapeHitTest().
enum class TapeHotspot { None, Open, Power, Rewind };

// Touch-sensitive areas of the Printer view -- see
// views_printerHitTest(). `value` is the switch position the touch
// asks for (Mode: 0 MAN, 1 TRACE, 2 NORM; Intensity: 0 LIGHT .. 4 DARK).
enum class PrinterHotspot { None, Power, Mode, Intensity, Print, Advance };
struct PrinterHit {
    PrinterHotspot spot = PrinterHotspot::None;
    int value = 0;
};

// Call once, after display/screen/plotter all exist (see hipi_init()).
// The Plotter view shows `hpgl` (the drawing) as the view of
// `plotterDevice` -- any plotter device with an HpglInterpreter.
// On the 7" panel this also loads hp82161a.bmp from the SD card into its own
// off-screen SDRAM layer (LT7683::kTapeLayerAddr) for the Tape view.
void views_init(DisplayDriver* display, Screen* screen, CDevice* plotterDevice, HpglInterpreter* hpgl);

// Tape view cassette (7" only; needs tape-in.bmp on the SD card): the
// cassette shows the selected .dat file's name on its label. Call
// views_setTapeFile() at boot and whenever another file is
// selected ("" = no file, empty drive). views_setTapeEjected(true)
// shows the drive empty with its lid open (open.bmp, if present)
// while the file picker is open. Both may be called
// from inside menu drawing -- they restore the active canvas themselves.
void views_setTapeFile(const std::string& filename);
void views_setTapeEjected(bool ejected);

// Tape view status LEDs (7" only; needs leds.bmp): POWER is lit while the
// drive device is enabled, BUSY while it's accessing its file (see
// CDrive::isBusy()). Updated from views_poll().
// Shows a short message (e.g. "Saved screendump_3.bmp") in the same
// yellow box as the view-switch splash, for 2.5 s. Taps are ignored
// meanwhile, like during the splash.
void views_showMessage(const char* text);

void views_setDrive(CDrive* drive);

// Printer view (7" only; needs hp82162a.bmp, and prtparts.bmp for the
// switches, POWER light and pressed keys): the printer shown. Its front
// panel follows the device by itself (views_poll()).
void views_setPrinter(CPrinter* printer);
CPrinter* views_printer();
// PRINT / PAPER ADVANCE from the view's keys: passed on to the printer,
// and the key is shown pressed (at least briefly, even for a quick tap)
void views_printerPrint(bool down);
void views_printerAdvance(bool down);

// TFDISPLAY's HP 92198 screen (video80.h). It draws itself; the view
// only switches it on and off.
void views_setVideo80(Video80* v);
Video80* views_video80();
// Puts the blinking text cursor back where the view showing has it
// (Screen's, or the HP 92198 view's) -- after something else has drawn
// text with the chip's one shared text cursor (menu, button strip, boxes)
void views_refreshCursor();
// The display's model (display.h): true = HP 92198, the Display view is
// then Video80 (output DisplayOutput::Video80) instead of Screen. If the
// display is showing, the other one takes over at once.
void views_setDisplayModel(bool v80);

// ── The demo (demo.h) ───────────────────────────────────────────────────
// Shows a view without the name box (dev: the device, for a device's view)
void views_showView(DisplayOutput mode, CDevice* dev);
// The Tape view as if the drive were working: POWER and BUSY lit, the
// reels turning (nothing happens to the drive itself)
void views_setDemoTape(bool busy);
// The printer's paper torn off (CPrinter::clearPaper()), redrawn empty
void views_clearPaper();
// The Printer view's paper, moved by hand (7"): a finger on the paper
// drags it up and down (boardui.cpp, touch_set_drag_callback()). Down
// shows older lines, back to the first one at the top; up pulls the
// newest line out above the tear-off bar, no further. Where it's let go
// it stays -- until something is printed (or PAPER ADVANCE, PRINT, X):
// then it glides back to the print head first.
bool views_paperDragBegin(int x, int y);   // true: on the paper, the touch moves it
void views_paperDragMove(int x, int y);
void views_paperDragEnd();
// Tearing off: the paper dragged up to the tear-off edge and then
// sideways (at least 100 px, finger still down). After the drag ends:
// true once, with the direction (-1 left, 1 right) -- the caller asks
// (Save / Discard / Cancel) and then calls views_tearOff().
bool views_takeTearGesture(int& dir);
// The paper above the tear-off edge slides away to that side, then the
// printer's paper is empty (CPrinter::clearPaper())
void views_tearOff(int dir);
int  views_paperLines();                   // lines on the paper (kept)
// The printer's paper was replaced (a printout loaded): drawn afresh,
// back at the print head
void views_reloadPaper();
void views_paperScrollBy(int lines);       // > 0: older lines (▲), < 0: newer (▼)
int  views_paperWindowLines();             // lines the window shows (Shift+▲/▼)
bool views_paperScrolled();                // not where it prints
void views_paperToLive();                  // back there (glides)
// The drive given to views_setDrive() (nullptr if none) -- e.g. for
// the power switch hotspot, which toggles it (see boardui_handleTap()).
CDrive* views_drive();

// True if the view can be shown on this build/board. Tape is only
// available on the 7" panel, and only if hp82161a.bmp loaded at boot;
// the same for Printer and hp82162a.bmp.
bool views_isAvailable(DisplayOutput mode);

// The kind of view currently shown
DisplayOutput views_output();

// Swipe (see boardui.cpp's boardui_handleSwipe()): shows the view of the next (forward) or previous device on the
// loop that has one (CDevice::viewKind()), in loop order.
void views_cycleOutput(bool forward);

// The devices that currently have a view (switched on, view available
// here), in loop order -- e.g. for the Display menu
std::vector<CDevice*> views_viewDevices();
// A view's name: "Display", "Plotter", "Tape" -- plus the device name if
// several devices have that kind of view ("Tape TFDRIVE2")
std::string views_viewTitle(CDevice* dev);
// Shows that device's view
void views_showDevice(CDevice* dev);
// At start-up: the view of the first device on the loop (in loop order)
// that is switched on and has one -- else the loop map. Also where HIPI goes
// when the view it would go back to (leaving the loop map, the hardware test
// ...) has gone, or no device has a view any more.
void views_showStartView();
// The device whose view is showing (nullptr if none)
CDevice* views_viewDevice();

// The view chosen from the HP-41 with "ESC # V <addr>" (sent to the
// display device): shows the view of the HIPI device at HP-IL address
// `addr`. requestViewByAddress() only records it (called from HP-IL frame
// handling); boardui_poll() picks it up with takeViewRequest() and runs
// showAddress() from the main loop. If there's no HIPI device at that
// address, or it has no view (or is disabled), nothing happens apart
// from a warning in the log. Returns true if the view was shown.
void views_requestViewByAddress(std::uint8_t addr);
bool views_takeViewRequest(std::uint8_t& addr);
bool views_showAddress(std::uint8_t addr);

// The HP-IL analyzer's view (analyzer.h) -- not a device, so not in the
// swipe order. Leaving it returns to the view shown before.
void views_showAnalyzer();
void views_leaveAnalyzer();

// The HP-IL loop map (loopmap.h) -- not a device, not in the swipe order;
// leaving it returns to the view shown before
void views_showLoopMap();
void views_leaveLoopMap();
// From the loop map's details box: a device's view, if it has one here
// (views_viewName(): "PLOTTER", "TAPE" ..., nullptr if none). X
// there brings back the loop map (views_backToLoopMap() -- true if
// it did); any other way of leaving the view forgets that.
const char* views_viewName(CDevice* dev);
void views_showDeviceFromLoopMap(CDevice* dev);
bool views_cameFromLoopMap();
bool views_backToLoopMap();

// HP-IL signal diagnostics (hpil_diag.h) -- like the loop map
void views_showSignals();
void views_leaveSignals();

// System -> Character table (chartable.h) -- a help page, like the loop map
// System -> Hardware test (hwtest.h) -- not a device's view; leaving it
// goes back to the view before
void views_showHwTest();
void views_leaveHwTest();
// A page: a view of HIPI's own with touch areas all over it -- Hardware
// test, Loop map, Media map, Character table, Analyzer, HP-IL signals. The
// X at the right of its title bar (or a swipe) leaves it; only its menu
// button in the bottom right corner brings up the button strip (on the
// other views the whole right edge does); no info box / device list
// corners, no shortcut at the left edge. page_chrome.h draws both buttons.
bool views_isPage(DisplayOutput o);
inline bool views_isPage() { return views_isPage(views_output()); }
// The page's X: back to the view before (its views_leave...())
void views_leavePage();
// Media map (mediamap.h): the drive's cassette/disk as rings; leaving goes
// back to the view before
void views_showMediaMap();
void views_leaveMediaMap();
void views_showCharTable();
void views_leaveCharTable();

// The clock screen saver (clock.h) -- shown and left without the splash
void views_showClock();
void views_leaveClock();

// The shortcut: a touch in the middle of the left edge (like the info box's
// top-left corner and the device list's bottom-left one) opens the view
// chosen in Settings -> Shortcut; touched again there, it goes back. The
// order is CONFIG.TXT's (Config::kShortcutNames) -- add new ones at the end.
enum class ShortcutView : std::uint8_t { LoopMap, Analyzer, Signals, CharTable, HwTest, Off };
constexpr int kShortcutViews = 6;
// The touch zone: kShortcutW wide from the left edge, kShortcutH high,
// centred between the two corners
constexpr int kShortcutW = 120, kShortcutH = 120;
// In the zone, and the view showing leaves it free (not the Hardware test
// page or the media map: their own buttons sit at the left edge)
bool views_shortcutTouch(int x, int y);
// Opens `v` -- or, while it's showing, goes back to the view before. False
// for Off (the touch is then nothing special)
bool views_openShortcut(ShortcutView v);
const char* views_shortcutName(ShortcutView v);   // "Loop map" ... (the menu)

// Call once per main-loop iteration -- handles the view-switch splash's
// auto-dismiss timer (see views_showDevice()).
void views_poll();

// True while the brief "DISPLAY"/"PLOTTER"/"TAPE" switch-announcement
// splash is showing (see views_showDevice()). Checked by
// UiDialog::close() (don't hide the PIP overlay the splash now owns) and
// by boardui_handleTap() (taps are ignored while it's up).
bool views_isSplashVisible();

// Clears the plotter's drawing (both the retained segments() history and,
// if Plotter output is currently showing, the on-screen drawing too) --
// the "new paper" menu action. Safe to call regardless of which output
// mode is currently showing.
void views_clearPlotter();

// Plot view: true = Fit (the plot itself fills the screen), false = Page
// (the whole P1..P2 area, like the paper). Display -> Plot view.
void views_setPlotFit(bool fit);
bool views_plotFit();

// Paper colour: false = white paper with dark pens (default), true =
// black paper with light pens (inverted). Plotter -> Paper.
void views_setPaperBlack(bool black);
bool views_paperBlack();
// The pen's tip (Plotter -> Pen): thin (1 pixel) or wide (3 pixels, a
// round tip), like putting a pen with a broader tip in the real plotter:
// only what's drawn from now on gets it, so thin and wide lines mix in one
// plot. Thin at start-up (not saved). HpglInterpreter::setWideTip().
void views_setPenWide(bool wide);
bool views_penWide();

// Redraws the current full-screen view from scratch: the plotter output
// (segments() replayed in full) or the Tape image. Called internally when
// switching views or when the menu closes back into one (to erase the
// menu box drawn on top), but exposed in case something else needs to
// force a refresh. Does nothing while Display (text) output is showing.
void views_redraw();

// Redraws only a rectangular sub-region (screen pixel coordinates) of the
// current full-screen view. For Tape this is a single BTE copy from the
// off-screen layer. For Plotter it narrows the active window to that region
// first, so replaying every segment only actually draws the ones (or
// parts of ones) that fall inside it, then restores the full-screen
// active window Plotter mode normally runs with. Used by boardui.cpp's
// hideButtonStrip() to restore just the area the button bitmap occupied,
// instead of a full-screen redraw for what's normally a small strip.
void views_redrawRegion(std::int16_t x0, std::int16_t y0,
                        std::int16_t w, std::int16_t h);

// True while a non-text full-screen view (Plotter or Tape) is showing,
// i.e. Screen is suspended and anything drawn over the view (such as the
// button strip) must be restored via views_redrawRegion().
bool views_isActive();

// Plotter digitizing (HP-GL DP): views_poll() brings up the Plotter
// view with a panel on the right -- a loupe, X/Y, arrows, ENTER, X. A touch
// on the plot puts the marker there, the arrows move it a pixel (held:
// repeated), ENTER gives the point to the plotter, X cancels.
bool views_digitizing();               // the panel is showing
bool views_digitizeTap(int x, int y);  // true: the touch was taken

// The plotter's ERROR light (the 7470A's front-panel LED), top centre of
// the Plotter view while HpglInterpreter::errorCode() isn't 0. A touch on it
// shows views_errorText() in a message box: true if it was there.
bool views_errorLightTap(int x, int y);
// Where the light is (its LED), for the demo's touch: false while it's out
bool views_errorLightPoint(int& x, int& y);
// "Error 3: bad parameter -- SC 0,100" (or "No error")
std::string views_errorText();
void views_digitizeRelease();          // finger up: an arrow stops repeating
// A strip button while digitizing (code as boardui: 2 OK = ENTER, 3/4 Up/
// Down, 5 X = cancel; repeat: held) -- true if taken
bool views_digitizeButton(int code, bool repeat);
// For tests: the centre of a panel control (0 up, 1 down, 2 left, 3 right,
// 4 ENTER, 5 X) and the marker, in screen pixels
bool views_digitizeControl(int id, int& x, int& y);
void views_digitizeMarker(int& x, int& y);
// Where a plotter point is on the screen, as the Plotter view draws it now
// (Page or Fit, narrowed while digitizing) -- the demo's "touch"
void views_plotToScreen(int px, int py, int& x, int& y);

// Returns which Tape hotspot (if any) contains the given screen point.
// Always TapeHotspot::None unless the Tape view is showing and its
// switch splash has timed out.
TapeHotspot views_tapeHitTest(std::uint16_t x, std::uint16_t y);

// Which Printer control (if any) is at the given screen point -- always
// PrinterHotspot::None unless the Printer view is showing and its switch
// splash has timed out.
PrinterHit views_printerHitTest(std::uint16_t x, std::uint16_t y);

}  // namespace hipi
