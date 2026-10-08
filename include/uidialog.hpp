// UiDialog.hpp
#pragma once
#include "display_config.h"
#include "sd_paths.h"
#include "Screen.hpp"
#include "ui_buttons.hpp"
#include "usb_serial.h"  // LOGF, used by applyTrace()/applyFile()
#include "hpil.h"        // CDevice, for the "Devices" enable/disable menu
#include "plotterview.h" // DisplayOutput, for the "Display" output-mode menu
#include "hpil_diag.h"    // BitCells, for the HP-IL signals "Bit cells" setting
#include "hipi_features.h"  // HIPI_MIRROR_PORT/TRANSPORT, for Settings -> Screen -> Mirror
#include "display_mirror.h" // displayMirror_setEnabled(), Settings -> Screen -> Mirror
#include "ff.h"
#include "pico/bootrom.h" // reset_usb_boot(), for the "Bootsel mode" menu item
#include "usb_msc.h"      // enterUsbMscMode()/exitUsbMscMode(), for "Connect to PC"
#include "drive.h"        // CDrive -- deferred switch-off in the Devices menu
#include "screendump.h"   // Display -> Screendump
#include "analyzer.h"     // Display -> Analyzer
#include "backlight.h"    // Settings -> Screen saver
#include "config.hpp"     // Settings -> Screen saver (saved settings)
namespace hipi { class Config; }
extern hipi::Config config;      // pico_main.cpp
#include "hipi.h"         // hipi_applyDeviceOrder(), Devices -> Change order
#include "lif_info.hpp"   // file picker: contents of a LIF .dat image
#include "loopback_result.h"  // LoopbackResult, for setLoopbackTestCallback()
#include "i2c_device.h"       // CI2CBus::scan(), for "Scan I2C"
#include "touch.h"            // touch_i2c, the bus "Scan I2C" scans
#include "glyph_font.h"       // menu titles in the HP-41 font

// Menu titles in the HP-41 display font (1) or the built-in font (0)
#ifndef HIPI_MENU_TITLE_HP41FONT
#define HIPI_MENU_TITLE_HP41FONT 1
#endif
namespace hipi { extern const GlyphFont kMenuTitleFont; }   // src/menu_font_title.cpp
#include <vector>
#include <algorithm>
#include <string>
#include <cstring>
#include <cstdio>
#include <cctype>
#include <functional>


// Global trace/debug flags, defined elsewhere (e.g. hipi.cpp) -- same
// convention as the existing global `SDOK`.
extern bool bTrace;
extern bool bExtTrace;

// The HP-IL device chain, defined in hipi.cpp -- the "Devices" menu lists
// these directly (by name()) and toggles their enabled() state.
extern std::vector<CDevice*> devices;

namespace hipi {

// Forward declaration -- defined in boardui.cpp. Not including the whole
// of boardui.h here since it already includes this file (uidialog.hpp).
void boardui_onMenuClosed();

// Shared box position/size/style for the on-screen menu frame. Pulled out
// to namespace scope so the splash screen (shown before any UiDialog
// exists) can draw an identical-looking frame without duplicating the
// yellow/radius/thickness constants.
namespace MenuFrame {
    // W widened to fit filenames up to ~32 characters at TextScale (16px/char
    // at scale 1) plus margins, while still staying clear of the button
    // strip at x=680.
    constexpr int X = 60, Y = 80, W = 580, H = 260;
    constexpr int BorderThickness = 3;
    constexpr int CornerRadius    = 14;

    // Same yellow/orange as used in buttons.bmp (#E8B800 -> RGB565).
    // Now that the display runs in genuine 16bpp mode, this renders as the
    // real, correct color -- no more of the 8bpp "green mod 8" quirk we
    // had to work around before (see git history / prior conversation).
    constexpr std::uint16_t Yellow = 0xEDC0;

    // The RA8875 only has one hardware text-scale register, shared by
    // Screen (HP-41 video emulation) and the menu. Screen can change it
    // at any time (font-size menu, or an incoming ESC sequence), so the
    // menu always forces it back to this fixed scale before drawing --
    // otherwise menu rows would grow/shrink along with the HP-41 font.
    constexpr std::uint8_t TextScale = 1;

    // Row pitch must fit TextScale's glyph height (16px base * (1+scale)
    // = 32px at scale 1) with a bit of breathing room.
    constexpr int RowPitch = 36;

    // The menus' title band, above the rows: the frame starts TitleH
    // higher than Y (rows still start at Y + 20)
    constexpr int TitleH = 30;
    constexpr int MenuTop = Y - TitleH;
    constexpr int MenuH = H + TitleH;

    // Thick, rounded frame: fill the whole box yellow (rounded corners),
    // then lay a smaller rounded black rectangle on top, inset by
    // BorderThickness on each side -> leaves an even yellow border.
    inline void draw(DisplayDriver* d, int x = X, int y = Y, int w = W, int h = H) {
        d->txtSize(TextScale);   // force menu's own scale, independent of Screen's
        // Every menu-style dialog in this project (the main menu itself,
        // showInfoBox(), showDeviceList() in boardui.cpp) calls
        // MenuFrame::draw() as its own first step, so putting this here
        // covers all of them with one call: menus always use the chip's
        // own built-in CGROM font (a standard, legible typeface), never
        // the custom hp82163_font.hpp glyphs Screen's own HP-41-emulated
        // text uses (see Screen.cpp's own selectCustomFont() call) --
        // matches this project's own "reassert explicitly, don't rely on
        // whichever mode was last active" convention (see txtSize()
        // just above, and Screen.cpp's own comment on why).
        d->selectBuiltinFont();
        // The four small corners outside the rounded shape show what's
        // BEHIND the box, so it really looks round (they used to be
        // filled black). On the 7" panel the box is drawn on the menu
        // layer, shown as a PIP window on top of the panel: what's behind
        // is first copied there from the panel (BTE, inside the display
        // chip) -- otherwise the corners would show stale layer memory. On
        // the 5" panel the box is drawn straight onto the screen.
#ifdef DISPLAY_7INCH
        if (d->canvasAddr() == LT7683::kMenuLayerAddr) {
            d->copyLayerRegion(0, SCREEN_MAX_X, static_cast<std::int16_t>(x), static_cast<std::int16_t>(y),
                               LT7683::kMenuLayerAddr, SCREEN_MAX_X, static_cast<std::int16_t>(x),
                               static_cast<std::int16_t>(y), static_cast<std::uint16_t>(w),
                               static_cast<std::uint16_t>(h));
        } else {
            d->fillRect(x, y, w, h, 0x0000);   // (another layer: no panel picture to show)
        }
#endif
        d->fillRoundRect(x, y, w, h, CornerRadius, Yellow);
        const int innerRadius = CornerRadius > BorderThickness
                                     ? CornerRadius - BorderThickness : 0;
        d->fillRoundRect(x + BorderThickness, y + BorderThickness,
                         w - 2 * BorderThickness, h - 2 * BorderThickness,
                         innerRadius, 0x0000);
    }
}  // namespace MenuFrame

// Note: the splash screen used to live here, but it's board "chrome" (not
// part of the interactive menu UiDialog implements) and has moved to
// boardui.h/.cpp alongside the button strip, info box, and status LEDs.

class UiDialog {
public:
    UiDialog(DisplayDriver* display, Screen& screen) : d_(display), screen_(screen) {}

    // Called with the newly chosen color whenever the user picks one in
    // the "Textcolor" menu (after screen_.setColor() has already applied
    // it). Useful for persisting the choice, e.g. to a Config object.
    void setColorChangedCallback(std::function<void(std::uint16_t)> cb) {
        onColorChanged_ = std::move(cb);
    }
    // Shift+Ok ("EXIT" on the button graphic) -- called whenever that
    // combo is pressed, whether or not a menu was actually open at the
    // time (see handleButton()'s own comment for why this now fires
    // unconditionally). boardui.cpp registers this to actually hide the
    // button strip -- UiDialog itself has no access to that (it lives
    // in a different file/namespace), only to its own menu state.
    // Shown in System -> About
    void setVersion(const char* v) { version_ = v; }

    void setExitRequestedCallback(std::function<void()> cb) {
        onExitRequested_ = std::move(cb);
    }
    // Called once at startup (from pico_main.cpp, right after config's
    // own filename() is known) so openFilePicker() can highlight the
    // file that was actually loaded at boot -- without this, only a
    // file picked LATER via the menu itself (which sets this same
    // member, in applyFile()) would ever get highlighted; the very
    // first time the menu is opened, before the user has ever used it
    // to pick anything, it fell back to the list's first entry
    // regardless of what was actually loaded.

    // The cassette drive the file picker works on: its current file is
    // pre-selected, the chosen file is given to it (CDrive::setMediaFile()),
    // and it has no cassette while the picker is open. With several
    // drives on the loop, each has its own file.
    void setTargetDrive(CDrive* drive) { targetDrive_ = drive; }
    CDrive* targetDrive() const { return targetDrive_; }

    // Called with the newly chosen (trace, debug) pair whenever the user
    // picks Off/On/Extended in the "Trace" menu (after the globals bTrace
    // and bExtTrace have already been updated). Useful for persisting the
    // choice.
    void setTraceChangedCallback(std::function<void(bool, bool)> cb) {
        onTraceChanged_ = std::move(cb);
    }

    // Called with the newly chosen font size (0..3) whenever the user
    // picks one in the "Font size" menu (after screen_.setTextSize() has
    // already applied it).
    void setFontSizeChangedCallback(std::function<void(std::uint8_t)> cb) {
        onFontSizeChanged_ = std::move(cb);
    }

    // Called with the newly chosen brightness (0..255) whenever the user
    // picks one in the "Brightness" menu (after screen_.setBrightness()
    // has already applied it).
    void setBrightnessChangedCallback(std::function<void(std::uint8_t)> cb) {
        onBrightnessChanged_ = std::move(cb);
    }

    // Called with the newly chosen column count (0 = auto) whenever the
    // user picks one in the "Columns" menu (after screen_.setColumns() has
    // already applied it).
    void setColumnsChangedCallback(std::function<void(std::uint8_t)> cb) {
        onColumnsChanged_ = std::move(cb);
    }

    // Called with a device's name() and its new enabled state whenever the
    // user toggles it in the "Devices" menu (after CDevice::setEnabled()
    // has already been applied). Useful for persisting the choice, e.g. to
    // a Config object.
    void setDeviceToggledCallback(std::function<void(const std::string&, bool)> cb) {
        onDeviceToggled_ = std::move(cb);
    }

    // Called to actually run the physical HP-IL loopback test (see
    // hipi.h's own hipi_loopbackTest()) when the user confirms the
    // "Loopback test" Config menu item's own dialog. Wired up by
    // pico_main.cpp, which is the only place that actually owns the
    // HpIlLoop instance this needs -- UiDialog itself has no business
    // knowing about the physical loop directly, same reasoning as every
    // other on*Changed_ callback here.
    //
    // The callback itself takes a progress-report function as its own
    // argument (matching hipi_loopbackTest()'s own onProgress parameter)
    // -- runLoopbackTest() below passes one in that redraws the dialog's
    // progress bar, called roughly once a second from partway through
    // the (otherwise fully synchronous, blocking) test itself.
    using LoopbackProgressFn = std::function<void(int tested, int total, std::uint32_t lastCmd)>;
    using LoopbackTestFn = std::function<LoopbackResult(LoopbackProgressFn)>;
    void setLoopbackTestCallback(LoopbackTestFn cb) {
        onLoopbackTest_ = std::move(cb);
    }

    bool isOpen() const { return state_ != State::Closed; }

    // Redraws the Config menu's "Connect to PC"/"Disconnect from PC" row
    // if that menu is showing -- called when the USB MSC mode changed
    // without going through the menu (the PC ejected the drive, see
    // usbMscPollHostEject()).
    void refreshUsbMscRow() {
        if (state_ != State::List || title_ != "SYSTEM") return;
#ifdef DISPLAY_7INCH
        d_->beginOverlayDraw();
#endif
        drawItems();
#ifdef DISPLAY_7INCH
        d_->endOverlayDraw();
        screen_.reassertRenderState();
        d_->showPipOverlay(MenuFrame::X, MenuFrame::MenuTop, MenuFrame::W, MenuFrame::MenuH);
#endif
    }

    // A message box (plotterview_showMessage()) was drawn over the open
    // menu in the shared PIP layer and has now timed out: put the menu back.
    // Lists are redrawn; any other dialog state can't be, so it's closed.
    void restoreAfterMessage() {
        if (state_ == State::Closed) return;
        if (state_ != State::List) { close(); return; }
#ifdef DISPLAY_7INCH
        d_->beginOverlayDraw();
#endif
        drawBox();
        drawItems();
#ifdef DISPLAY_7INCH
        d_->endOverlayDraw();
        screen_.reassertRenderState();
        d_->showPipOverlay(MenuFrame::X, MenuFrame::MenuTop, MenuFrame::W, MenuFrame::MenuH);
#endif
    }

    // Opens the .dat file picker directly, without going through the main
    // and Config menus -- used by the Tape view's cassette window / OPEN
    // hotspots (see boardui_handleTap()). X or a confirmed selection closes
    // the menu again, back to the Tape view. No-op if a menu is already open.
    void openFilePickerFromTape() {
        if (state_ != State::Closed) return;
        pickerFromTape_ = true;
#ifndef DISPLAY_7INCH
        screen_.suspend();   // same as menuOpening()
#endif
        d_->setTextCursorVisible(false, false);
#ifdef DISPLAY_7INCH
        d_->beginOverlayDraw();
#endif
        openFilePicker();
#ifdef DISPLAY_7INCH
        d_->endOverlayDraw();
        screen_.reassertRenderState();
        d_->showPipOverlay(MenuFrame::X, MenuFrame::MenuTop, MenuFrame::W, MenuFrame::MenuH);
#endif
    }

    // Used by boardui.cpp's boardui_handleTap() to detect/dismiss the
    // loopback test's own result dialog on any touch -- same pattern as
    // its own infoBoxVisible/deviceListVisible checks, just routed
    // through UiDialog since this result IS one of its own menu states
    // (see openLoopbackConfirm()/runLoopbackTest() further down) rather
    // than a separate corner-tap box like infoBox/deviceList are.
    // ── Warning box ──────────────────────────────────────────────────────
    // A box with a warning (e.g. PILBox in CON mode: no frame came back, or
    // another controller on the loop) that stays until the screen is
    // touched. Called from the main loop; does nothing while a menu or
    // another box is open (the warning is in the log anyway).
    void showWarning(const char* title, const char* line1, const char* line2 = "") {
        if (state_ != State::Closed) return;
#ifndef DISPLAY_7INCH
        screen_.suspend();
#endif
        d_->setTextCursorVisible(false, false);
#ifdef DISPLAY_7INCH
        d_->beginOverlayDraw();
#endif
        state_ = State::Warning;
        title_ = title;
        drawBox();
        d_->txtSize(0);
        d_->txtColor(0xFFFF, 0x0000);
        d_->txtSetCursor(MenuFrame::X + 20, MenuFrame::Y + 20);
        d_->txtWrite(line1);
        d_->txtSetCursor(MenuFrame::X + 20, MenuFrame::Y + 44);
        d_->txtWrite(line2);
        d_->txtColor(0x7BEF, 0x0000);
        d_->txtSetCursor(MenuFrame::X + 20, MenuFrame::Y + MenuFrame::H - 40);
        d_->txtWrite("Touch the screen to close");
#ifdef DISPLAY_7INCH
        d_->endOverlayDraw();
        screen_.reassertRenderState();
        d_->showPipOverlay(MenuFrame::X, MenuFrame::MenuTop, MenuFrame::W, MenuFrame::MenuH);
#endif
    }
    bool isShowingWarning() const { return state_ == State::Warning; }
    void dismissWarning() {
        if (state_ != State::Warning) return;
        close();
    }

    bool isShowingLoopbackResult() const { return state_ == State::LoopbackResult; }
    void dismissLoopbackResult() {
        if (state_ != State::LoopbackResult) return;
        // Called from boardui_handleTap() -- a completely separate call
        // path from handleButton(), which normally provides the
        // beginOverlayDraw()/endOverlayDraw()+showPipOverlay() wrapping
        // every other menu-drawing call in this file relies on (see
        // handleButton()'s own end-of-function sequence). openSystemMenu()
        // below draws menu content exactly like any other open*Menu(), so
        // it needs that same wrapping here explicitly, or it would (on
        // the 7" panel) draw straight to the main window's canvas instead
        // of the PIP layer -- confirmed as corrupting the live content
        // underneath rather than showing the Config menu at all.
#ifdef DISPLAY_7INCH
        d_->beginOverlayDraw();
#endif
        openSystemMenu();
#ifdef DISPLAY_7INCH
        d_->endOverlayDraw();
        screen_.reassertRenderState();
        d_->showPipOverlay(MenuFrame::X, MenuFrame::MenuTop, MenuFrame::W, MenuFrame::MenuH);
#endif
    }

    // Same pattern as isShowingLoopbackResult()/dismissLoopbackResult()
    // just above (see their own comments for the full reasoning) --
    // Config > "Scan I2C" runs instantly (no confirmation dialog needed;
    // scanning is quick and non-destructive, unlike the loopback test's
    // own physical-jumper requirement) and shows its own result grid,
    // dismissed by touch.
    bool isShowingI2CScanResult() const { return state_ == State::I2CScanResult; }
    void dismissI2CScanResult() {
        if (state_ != State::I2CScanResult) return;
#ifdef DISPLAY_7INCH
        d_->beginOverlayDraw();
#endif
        openSystemMenu();
#ifdef DISPLAY_7INCH
        d_->endOverlayDraw();
        screen_.reassertRenderState();
        d_->showPipOverlay(MenuFrame::X, MenuFrame::MenuTop, MenuFrame::W, MenuFrame::MenuH);
#endif
    }

    // Called from the main loop when a button touch is detected.
    // Shift+Ok closes the menu from any depth (see the check right
    // below). A Shift tap is consumed just like any other button press
    // by the "touch that woke the strip is consumed" skip in
    // boardui.cpp -- confirmed intentional/expected: waking the strip
    // and pressing a button are deliberately kept as two separate taps,
    // so the very first tap after the strip's been hidden never presses
    // anything, Shift included, even if it happens to land on the
    // Shift button's own position.
    // Wraps a single call to a Screen-mutating method that ends in a full
    // redraw (setTextSize()/setColumns()/clear(), all of which call
    // Screen::full() internally) -- these can be triggered from WITHIN
    // the menu's own button handling (Font size/Columns/"Clear screen"),
    // which on the 7" panel runs with the canvas still pointed at the
    // menu's own overlay layer (see handleButton()'s own
    // beginOverlayDraw()/endOverlayDraw() wrapping). Without this,
    // full()'s entire "redraw every visible line" pass -- which is
    // exactly what's needed to re-render EXISTING text at a newly
    // picked font size -- silently drew to the invisible menu layer
    // instead of the main window: confirmed on real hardware as the
    // cursor moving and new text appearing at the new size, but already-
    // written text staying at the old size until something else (the
    // next natural scroll, once the menu had closed and the canvas was
    // back to normal) finally redrew it correctly. Temporarily restores
    // the canvas to the main window for just the one call, then switches
    // back to the menu layer afterward so the rest of the calling
    // function (e.g. openSettingsMenu(), called right after
    // applyFontSize() returns) can keep drawing the menu itself
    // normally. On the 5" panel, where none of this canvas-switching
    // concept exists, this just calls fn() directly.
    template <typename F>
    void withMainCanvas(F&& fn) {
#ifdef DISPLAY_7INCH
        d_->endOverlayDraw();
        fn();
        d_->beginOverlayDraw();
#else
        fn();
#endif
    }

    // repeat: an auto-repeat of a held ▲/▼ (see boardui_poll()) -- acts
    // with the same Shift state as the original press (line vs page)
    void handleButton(Button b, bool repeat = false) {
        if (b == Button::Shift) {
            shiftPending_ = true;   // latch for the *next* button press
            return;
        }
        const bool shifted = repeat ? lastShifted_ : shiftPending_;
        if (!repeat) {
            shiftPending_ = false;  // consumed by this button press, whatever it is
            lastShifted_ = shifted;
        }

        // Shift+OK ("EXIT" on the button graphic) always means "leave
        // entirely and hide the buttons" -- whether or not a menu
        // happens to be open at the time. Previously required isOpen(),
        // so pressing Shift+Ok with the strip showing but no menu open
        // (e.g. right after the strip's own wake tap, before a
        // separate Ok has actually opened anything) fell through to
        // ordinary Ok handling instead (opening the menu, since
        // Button::Ok normally does that from State::Closed) -- confirmed
        // as genuinely confusing, not the intended behaviour. close()
        // itself is harmless to call even when already closed (just a
        // redundant, cheap re-resume/redraw), so no isOpen() guard is
        // needed here at all any more.
        if (shifted && b == Button::Ok) {
            close();
#ifdef DISPLAY_7INCH
            // Canvas is still address 0 here (this path returns before
            // ever calling beginOverlayDraw()), so it's safe to restore
            // the cursor directly -- see the end-of-function version of
            // this same call for the full reasoning on why it can't run
            // any earlier than "canvas is definitely back at the main
            // window".
            screen_.refreshCursor();
#endif
            if (onExitRequested_) onExitRequested_();
            return;
        }

#ifdef DISPLAY_7INCH
        // Every state OTHER than Closed only ever draws menu content
        // (highlighting a row, opening a sub-menu, closing) -- Closed
        // itself is the one case that can go either way: Ok opens the
        // main menu (menu content), but Up/Down/X there scroll or clear
        // the live text screen instead (Screen::scrollBy()/clear()/
        // scrollToLive() -- NOT menu content, must NOT be redirected to
        // the menu layer). willDrawMenu covers exactly the cases that
        // actually draw the menu, matching the switch below case by
        // case.
        const bool willDrawMenu = !(state_ == State::Closed && b != Button::Ok);
        if (willDrawMenu) d_->beginOverlayDraw();
#endif

        switch (state_) {
            case State::Warning:                // also closed by any touch (boardui.cpp)
                if (b == Button::Ok || b == Button::X) close();
                break;
            case State::Closed:
                if (plotterview_output() == DisplayOutput::LoopMap && b == Button::X) {
                    // The loop map: X leaves it (back to the view before)
                    plotterview_leaveLoopMap();
                } else if (plotterview_output() == DisplayOutput::Signals && b == Button::X) {
                    plotterview_leaveSignals();
                } else if (plotterview_output() == DisplayOutput::CharTable) {
                    withMainCanvas([] { plotterview_leaveCharTable(); });   // any button closes it
                } else if (plotterview_output() == DisplayOutput::Analyzer) {
                    // The Analyzer view: its own menu, and the arrows scroll
                    // its log instead of the HP-41 text
                    if (b == Button::Ok)        openContextMenu();
                    else if (b == Button::Up)   analyzer_scroll(shifted ? analyzer_pageRows() : 1);
                    else if (b == Button::Down) analyzer_scroll(shifted ? -analyzer_pageRows() : -1);
                    else if (b == Button::X) {
                        if (shifted) analyzer_clear();
                        else         analyzer_scrollToLive();
                    }
                } else if (b == Button::Ok) {
                    openContextMenu();
                } else if (b == Button::Up) {
                    // Up = scroll further into history (older content).
                    // With Shift: a whole page (one screen's worth of rows).
                    screen_.scrollBy(shifted ? screen_.rows() : 1);
                } else if (b == Button::Down) {
                    // Down = scroll back toward the live view (newer content).
                    screen_.scrollBy(shifted ? -screen_.rows() : -1);
                } else if (b == Button::X) {
                    // X = jump to the bottom (live view). Shift+X = clear.
                    if (shifted) screen_.clear();
                    else         screen_.scrollToLive();
                }
                break;

            case State::List:
                if (b == Button::Up)   moveItemSelection(-1);
                if (b == Button::Down) moveItemSelection(+1);
                if (b == Button::Ok && !items_.empty())
                    items_[static_cast<std::size_t>(selected_)].ok();
                if (b == Button::X) {
                    if (back_) { auto bk = back_; bk(); }
                    else       close();
                }
                break;

            case State::FilePicker:
                if (b == Button::Up)   moveFileSelection(-1);
                if (b == Button::Down) moveFileSelection(+1);
                if (b == Button::Ok && !files_.empty()) openConfirmFile(files_[selected_]);
                if (b == Button::X) {
                    // Opened from the Tape view: back means straight back to it
                    if (pickerFromTape_) close();
                    else                 openSystemMenu();
                }
                break;

            case State::ConfirmFile:
                if (b == Button::Ok) {
                    applyFile(pendingFile_);
                    if (pickerFromTape_) close();
                    else                 openSystemMenu();
                }
                if (b == Button::Down && !pendingFile_.empty()) openLifInfo();
                if (b == Button::X)  returnToFilePicker();
                break;

            case State::LifInfo:
                // Up/Down: one line, Shift+Up/Down: one page
                if (b == Button::Up)   scrollLifInfo(shifted ? -kLifInfoRows : -1);
                if (b == Button::Down) scrollLifInfo(shifted ?  kLifInfoRows :  1);
                if (b == Button::Ok) {                  // OK: choose this file
                    applyFile(pendingFile_);
                    if (pickerFromTape_) close();
                    else                 openSystemMenu();
                }
                if (b == Button::X)  returnToFilePicker();
                break;

            case State::DeviceList:
                if (b == Button::Up)   moveDeviceSelection(-1);
                if (b == Button::Down) moveDeviceSelection(+1);
                if (b == Button::Ok)   toggleDevice(selected_);
                if (b == Button::X)    openDevicesMenu(0);
                break;

            case State::DeviceOrder:
                // Pick a device with Up/Down + OK, then move it with Up/Down;
                // OK keeps the new position, X puts it back
                if (b == Button::Up)   orderMove(-1);
                if (b == Button::Down) orderMove(+1);
                if (b == Button::Ok) {
                    if (orderMoving_) {
                        orderMoving_ = false;
                        hipi_applyDeviceOrder(orderList_);
                    } else {
                        orderMoving_ = true;
                        orderPickedFrom_ = selected_;
                    }
                    drawDeviceOrder();
                }
                if (b == Button::X) {
                    if (orderMoving_) {             // cancel this move
                        orderMoving_ = false;
                        orderList_ = devices;
                        selected_ = orderPickedFrom_;
                        keepOrderSelectionVisible();
                        drawDeviceOrder();
                    } else {
                        openDevicesMenu(1);         // back to Devices
                    }
                }
                break;

            case State::AnalyzerLogMenu:
                if (b == Button::Up)   moveListSelection(analyzerLogLabels_, analyzerLogScroll_, -1);
                if (b == Button::Down) moveListSelection(analyzerLogLabels_, analyzerLogScroll_, +1);
                if (b == Button::Ok)   enterAnalyzerLogMenuItem();
                if (b == Button::X)    openContextMenu(2);
                break;

            case State::LoopbackConfirm:
                if (b == Button::Ok) runLoopbackTest();
                if (b == Button::X)  openSystemMenu();
                break;

            case State::LoopbackResult:
                // Primarily dismissed by a touch, anywhere on screen --
                // see boardui.cpp's boardui_handleTap(), which checks
                // isShowingLoopbackResult()/dismissLoopbackResult() for
                // this. Ok/X here too, for anyone navigating by buttons
                // alone.
                if (b == Button::Ok || b == Button::X) openSystemMenu();
                break;

            case State::I2CScanResult:
                // Same "primarily touch, Ok/X also work" pattern as
                // State::LoopbackResult just above -- see boardui.cpp's
                // own isShowingI2CScanResult()/dismissI2CScanResult() check.
                if (b == Button::Ok || b == Button::X) openSystemMenu();
                break;
        }

#ifdef DISPLAY_7INCH
        if (willDrawMenu) {
            d_->endOverlayDraw();
            // MenuFrame::draw() (called by every open*Menu()) and the
            // menu's own row/highlight drawing (drawRow(), drawColorRow(),
            // fillRect()-based highlight backgrounds, etc.) all set the
            // hardware text-size and foreground-colour registers to
            // their OWN values -- both are single, global registers, not
            // tied to which canvas is currently selected, so they stay
            // at whatever the menu last set them to even after switching
            // the canvas back to the main window above. Previously
            // harmless (Screen was suspended the whole time a menu was
            // open, so it never drew anything in between), but now that
            // Screen keeps drawing live even while the menu is open (any
            // HP-IL data arriving asynchronously), leaving these
            // un-restored made whatever Screen drew next silently render
            // at the menu's own font size/colour instead of Screen's own
            // -- confirmed on real hardware (text size first, then
            // colour and the menu's own highlight backgrounds bleeding
            // into Screen's text too). reassertRenderState() re-applies
            // both (two register writes, not a layout recalculation or
            // redraw) every time the menu has drawn anything at all,
            // whether it just opened, is still open (navigating between
            // items), or just closed.
            screen_.reassertRenderState();
            if (state_ != State::Closed) {
                // Show (or refresh, if already showing -- harmless,
                // idempotent) the freshly-drawn menu content as a PIP
                // overlay. If state_ IS Closed here, this button press
                // just closed the menu (X, or Shift+Ok handled earlier)
                // -- close() already called hidePipOverlay() itself,
                // nothing more to do.
                d_->showPipOverlay(MenuFrame::X, MenuFrame::MenuTop, MenuFrame::W, MenuFrame::MenuH);
            } else {
                // menuOpening() (every menu opening from closed) hides the
                // hardware blinking cursor before drawing -- restore it
                // now that the canvas is safely back at the main window
                // (CURH/CURV, which the cursor's own position rides on,
                // are interpreted relative to whatever the canvas
                // currently is -- doing this any earlier, while the
                // canvas was still the menu layer, would have positioned
                // it there instead of the main window). refreshCursor()
                // re-applies Screen's own last-known visibility/position
                // without a full redraw (nothing to redraw anyway -- the
                // main window's own content was never touched).
                screen_.refreshCursor();
            }
        }
#endif
    }

private:
#ifdef DISPLAY_7INCH
    static constexpr bool kHasScreendump = true;     // (no read-back on the 5" panel)
#else
    static constexpr bool kHasScreendump = false;
#endif

    enum class State {
        Closed, List,                       // List: every menu (see openList())
        FilePicker, ConfirmFile, DeviceList,
        LoopbackConfirm, LoopbackResult, I2CScanResult, LifInfo, DeviceOrder,
        AnalyzerLogMenu, Warning
    };




    static constexpr std::uint16_t kColors[]     = { 0xFFFF, 0xFFE0, 0x07E0, 0x07FF, 0xF800 };
    static constexpr const char*   kColorLabels[] = { "White", "Yellow", "Green", "Cyan", "Red" };
    static constexpr int kColorCount = 5;

    // Matches Screen's size_ (0..3, built-in CGRAM modes only -- the
    // custom "fon" mode 4 isn't offered here).
    static constexpr int kFontSizeCount = 4;

    // A handful of discrete steps rather than a continuous 0..255 slider,
    // since navigation is just Up/Down/Ok/X.
    static constexpr std::uint8_t  kBrightnessLevels[] = { 51, 102, 153, 204, 255 };
    static constexpr const char*   kBrightnessLabels[] = { "20%", "40%", "60%", "80%", "100%" };
    static constexpr int kBrightnessCount = 5;

    // 0 = auto (max for current font size *and* current text width -- the
    // button strip hiding/showing changes that at runtime, see
    // boardui.cpp's showButtonStrip()/hideButtonStrip()). 32 = the
    // original HP82163's column count. The rest are the natural max at
    // each font size (0-3) at the *full* 800px panel width (i.e. with the
    // button strip hidden) -- if the strip happens to be showing when one
    // of these is picked, Screen::setColumns() clamps it down to whatever
    // the narrower width currently allows instead of overflowing.
    static constexpr std::uint8_t  kColumnsValues[] = { 0, 25, 32, 33, 50, 100 };
    static constexpr const char*   kColumnsLabels[] = { "Auto", "25", "32", "33", "50", "100" };
    static constexpr int kColumnsCount = 6;

    // Off = bTrace/bExtTrace both false. On = bTrace only. Extended = both.
    static constexpr const char* kTraceLabels[] = { "Off", "On", "Extended" };
    static constexpr int kTraceCount = 3;

    // "Display"/"Plotter" pick which full-screen output is showing (see
    // plotterview.h); "Clear plotter" is an immediate action ("new paper"),
    // not a pickable state, so it doesn't need a selected_-tracked value.



    // ── Analyzer menu (the menu button while the Analyzer view is shown) ─




    // Analyzer -> Logging: Start/Stop (a new file each time), Save (the
    // current buffer to a new file, while not logging). X: back.
    std::vector<std::string> analyzerLogLabels_;
    int analyzerLogScroll_ = 0;

    void openAnalyzerLogMenu() {
        state_ = State::AnalyzerLogMenu;
        selected_ = 0;
        analyzerLogScroll_ = 0;
        analyzerLogLabels_ = { analyzer_logging() ? "Stop" : "Start", "Save" };
        title_ = "ANALYZER / LOGGING";
        drawBox();
        drawListRows(analyzerLogLabels_, analyzerLogScroll_);
    }

    void enterAnalyzerLogMenuItem() {
        std::string msg;
        std::string shown;
        if (selected_ == 0) {
            if (analyzer_logging()) {
                analyzer_stopLog();
                shown = "Logging stopped";
            } else {
                shown = analyzer_startLog(msg) ? "Logging to " + msg : msg;
            }
        } else {
            shown = analyzer_saveLog(msg) ? "Saved " + msg : msg;
        }
        withMainCanvas([&]{ plotterview_showMessage(shown.c_str()); });
        close();
    }

    // Generic scrolling list rows (labels, first row shown) -- same look
    // as the other lists
    void drawListRows(const std::vector<std::string>& labels, int scroll) {
        d_->txtSize(MenuFrame::TextScale);
        for (int row = 0; row < kMaxFilesShown; ++row) {
            const std::size_t i = static_cast<std::size_t>(scroll + row);
            const bool has = i < labels.size();
            const bool sel = has && static_cast<int>(i) == selected_;
            _clearRowBackground(row, sel ? MenuFrame::Yellow : 0x0000);
            if (!has) continue;
            rowFg_ = sel ? 0x0000 : 0xFFFF;
            d_->txtColor(rowFg_, sel ? MenuFrame::Yellow : 0x0000);
            writeRowLabel(MenuFrame::Y + 20 + row * MenuFrame::RowPitch, labels[i].c_str());
        }
        drawScrollIndicators(scroll, static_cast<int>(labels.size()));
    }
    void moveListSelection(const std::vector<std::string>& labels, int& scroll, int delta) {
        const int count = static_cast<int>(labels.size());
        if (count == 0) return;
        selected_ = (selected_ + delta + count) % count;
        if (selected_ < scroll) scroll = selected_;
        else if (selected_ >= scroll + kMaxFilesShown) scroll = selected_ - kMaxFilesShown + 1;
        drawListRows(labels, scroll);
    }






    // Body of the "Connected" message (box already drawn, first line's
    // cursor already set)
    void drawConnectedText() {
        d_->txtWrite("Connected -- SD card is now");
        d_->txtSetCursor(MenuFrame::X + 20, MenuFrame::Y + 20 + MenuFrame::RowPitch);
        d_->txtWrite("visible as a USB drive on the PC.");
        d_->txtSetCursor(MenuFrame::X + 20, MenuFrame::Y + 20 + 2 * MenuFrame::RowPitch);
        d_->txtWrite("HP-IL drive access paused.");
    }

    bool devForceDisconnectLabel_ = false;   // HIPI_DEV_SCREENDUMPS only

#if HIPI_DEV_SCREENDUMPS
    // Screen dump with menus of whatever the menu layer shows right now
    // (see HIPI_DEV_SCREENDUMPS in screendump.h)
    void devScreendump(const char* what) {
        std::string msg;
        const bool ok = screendump_save(d_, msg, /*withOverlays=*/true);
        LOGF("\r\n * DEV screendump (%s): %s%s", what, ok ? "saved " : "FAILED -- ", msg.c_str());
    }

    // The two screens seen while connected to the PC, drawn and dumped
    // before actually connecting: the "Connected" message, and the Config
    // menu with "Disconnect from PC" selected
    void devDumpConnectScreens() {
        drawBox();
        d_->txtColor(0xFFFF, 0x0000);
        d_->txtSetCursor(MenuFrame::X + 20, MenuFrame::Y + 20);
        drawConnectedText();
        devScreendump("Connected message");

        const int keep = selected_;
        devForceDisconnectLabel_ = true;
        selected_ = 0;                      // "Disconnect from PC" highlighted
        drawBox();
        drawItems();
        devScreendump("System menu, Disconnect from PC");
        devForceDisconnectLabel_ = false;
        selected_ = keep;
    }
#endif


    // "Scan I2C" -- runs immediately (no confirmation needed; scanning
    // is quick, ~112 short probes, and non-destructive to whatever's on
    // the bus) and shows the result grid, dismissed by touch (see
    // isShowingI2CScanResult()/dismissI2CScanResult()) or Ok/X.
    void runI2CScan() {
        state_ = State::I2CScanResult;
        title_ = "SYSTEM / I2C SCAN";
        drawBox();
        d_->txtColor(0xFFFF, 0x0000);
        d_->txtSize(MenuFrame::TextScale);
        d_->txtSetCursor(MenuFrame::X + 20, MenuFrame::Y + 20);
        d_->txtWrite("Scanning...");

        bool found[128] = { false };
        CI2CBus::scan(touch_i2c, found);

        // Clears "Scanning..." (drawn above at MenuFrame::TextScale, a
        // larger/taller glyph) before the grid below switches to its own
        // smaller scale -- without this, the smaller grid text doesn't
        // fully cover the larger text's own footprint, leaving visible
        // remnants of "Scanning..." peeking out around/under the grid.
        // Same "redraw the box before showing the final content" pattern
        // runLoopbackTest() already uses between its own "Testing..." and
        // result text.
        drawBox();
        drawI2CScanGrid(found);
    }

    // Renders the classic i2cdetect-style grid: a header row of column
    // nibbles (0-f), then one row per address decade (00, 10, 20, ...)
    // with each cell showing the full 2-digit hex address if that
    // address acknowledged, "--" if it didn't (but was actually probed),
    // or left blank for the reserved 0x00-0x07/0x78-0x7f range CI2CBus::
    // scan() deliberately never touches -- immediately recognisable to
    // anyone who's used the Linux i2cdetect tool, rather than inventing
    // a new layout. Uses a smaller text scale than the rest of the menu
    // (this project's own txtSize() scale 0, the smallest available --
    // see LT7683::txtSize()/RA8875::txtSize()) purely for this one
    // screen, restored to MenuFrame::TextScale by whatever's drawn next
    // (every other open*Menu() already sets its own scale explicitly
    // before drawing, so nothing else needs to know or care this dialog
    // used a different one).
    void drawI2CScanGrid(const bool found[128]) {
        constexpr std::uint8_t kGridScale = 0;
        d_->txtColor(0xFFFF, 0x0000);
        d_->txtSize(kGridScale);
        const int lineHeight = 18;  // a little taller than the scale-0 glyph's own 16px,
                                     // for readability -- matches this dialog's own grid
                                     // rows below, nothing else on screen uses this pitch
        const int gridX = MenuFrame::X + 20;
        int y = MenuFrame::Y + 20;

        char line[64];
        int foundCount = 0;
        for (int a = 0x08; a <= 0x77; ++a) if (found[a]) ++foundCount;
        std::snprintf(line, sizeof(line), "Found %d device(s):", foundCount);
        d_->txtSetCursor(gridX, y);
        d_->txtWrite(line);
        y += lineHeight + 4;

        d_->txtSetCursor(gridX, y);
        d_->txtWrite("     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f");
        y += lineHeight;

        for (int row = 0; row < 8; ++row) {
            int pos = std::snprintf(line, sizeof(line), "%02x: ", row * 16);
            for (int col = 0; col < 16; ++col) {
                const int addr = row * 16 + col;
                if (addr < 0x08 || addr > 0x77) {
                    pos += std::snprintf(line + pos, sizeof(line) - static_cast<std::size_t>(pos), "   ");
                } else if (found[addr]) {
                    pos += std::snprintf(line + pos, sizeof(line) - static_cast<std::size_t>(pos),
                                         "%02x ", addr);
                } else {
                    pos += std::snprintf(line + pos, sizeof(line) - static_cast<std::size_t>(pos), "-- ");
                }
            }
            d_->txtSetCursor(gridX, y);
            d_->txtWrite(line);
            y += lineHeight;
        }

        y += 4;
        d_->txtSize(MenuFrame::TextScale);
        d_->txtSetCursor(gridX, y);
        d_->txtWrite("Touch anywhere to continue.");
    }

    // "Loopback test" -- first shows a confirmation dialog explaining
    // the physical jumper it needs (Button::Ok proceeds to actually run
    // it; Button::X backs out to the Config menu without running
    // anything).
    void openLoopbackConfirm() {
        state_ = State::LoopbackConfirm;
        title_ = "SYSTEM / LOOPBACK TEST";
        drawBox();
        d_->txtColor(0xFFFF, 0x0000);
        d_->txtSize(MenuFrame::TextScale);
        d_->txtSetCursor(MenuFrame::X + 20, MenuFrame::Y + 20);
        d_->txtWrite("Connect the HP-IL loop cable,");
        d_->txtSetCursor(MenuFrame::X + 20, MenuFrame::Y + 20 + MenuFrame::RowPitch);
        d_->txtWrite("then press OK to test.");
        d_->txtSetCursor(MenuFrame::X + 20, MenuFrame::Y + 20 + 3 * MenuFrame::RowPitch);
        d_->txtWrite("X to cancel.");
    }

    // Draws (or redraws) just the progress line + bar -- called both to
    // set up the initial 0% state and, roughly once a second, from
    // partway through the test itself (via the onProgress callback
    // passed into onLoopbackTest_() below). Only touches the two rows it
    // owns (text line + bar), not the whole box, so it doesn't need
    // drawBox() run again each time.
    void drawLoopbackProgress(int tested, int total, std::uint32_t lastCmd) {
        const int pct = total > 0 ? (tested * 100) / total : 0;
        char buf[48];
        std::snprintf(buf, sizeof(buf), "Testing... %d%% (0x%03X)",
                      pct, static_cast<unsigned>(lastCmd));

        const int textX = MenuFrame::X + 20, textY = MenuFrame::Y + 20;
        d_->fillRect(textX, textY, MenuFrame::W - 40, MenuFrame::RowPitch, 0x0000);
        d_->txtColor(0xFFFF, 0x0000);
        d_->txtSize(MenuFrame::TextScale);
        d_->txtSetCursor(textX, textY);
        d_->txtWrite(buf);

        const int barX = MenuFrame::X + 20, barY = MenuFrame::Y + 20 + MenuFrame::RowPitch;
        const int barW = MenuFrame::W - 40, barH = 20;
        d_->fillRect(barX, barY, barW, barH, 0x0000);
        d_->rect(barX, barY, barW, barH, 0xFFFF);
        const int fillW = (barW - 2) * pct / 100;
        if (fillW > 0) d_->fillRect(barX + 1, barY + 1, fillW, barH - 2, 0x07E0);
    }

    // Actually runs the test (via the callback pico_main.cpp wired up --
    // see setLoopbackTestCallback()) and shows the result. This call
    // blocks for as long as the test itself takes; drawLoopbackProgress()
    // above is called roughly once a second from partway through it (see
    // hipi_loopbackTest()'s own onProgress parameter) so the bar/percent
    // actually advances instead of the screen just looking frozen.
    // Stops at the very first failure rather than working through the
    // whole range regardless -- confirmed painfully slow otherwise with
    // nothing connected (every one of ~1000 values timing out in turn).
    // Touch is the primary way to dismiss the result afterward (see
    // boardui.cpp's own boardui_handleTap(), which checks
    // isShowingLoopbackResult()/dismissLoopbackResult() for this) --
    // Button::Ok/X in State::LoopbackResult below also work, for anyone
    // navigating by buttons alone.
    void runLoopbackTest() {
        state_ = State::LoopbackResult;
        drawBox();
        drawLoopbackProgress(0, 1, 0);

        const LoopbackResult result = onLoopbackTest_
            ? onLoopbackTest_([this](int tested, int total, std::uint32_t lastCmd) {
                  drawLoopbackProgress(tested, total, lastCmd);
              })
            : LoopbackResult{0, 0, 0};

        drawBox();  // clear the progress line/bar before showing the result
        d_->txtColor(0xFFFF, 0x0000);
        d_->txtSize(MenuFrame::TextScale);
        d_->txtSetCursor(MenuFrame::X + 20, MenuFrame::Y + 20);
        char buf[48];
        if (result.errors == 0) {
            std::snprintf(buf, sizeof(buf), "Loopback OK! (%d/%d)", result.tested, result.tested);
        } else {
            std::snprintf(buf, sizeof(buf), "Loopback FAILED at 0x%03X",
                          static_cast<unsigned>(result.failedCmd));
        }
        d_->txtWrite(buf);
        d_->txtSetCursor(MenuFrame::X + 20, MenuFrame::Y + 20 + 2 * MenuFrame::RowPitch);
        d_->txtWrite("Touch anywhere to continue.");
    }




    // ── Settings -> Screen saver ─────────────────────────────────────────
    // After 10 minutes without use: dim, show the clock (clock.h), or
    // nothing; and how the clock looks. OK changes the selected setting
    // (saved at once); "Show now" starts the screen saver right away.




    // Config -> Bootsel mode
    void enterBootselMode() {
        // Bootsel mode -- immediate action (like "Clear plotter" in
        // the Display menu), not a pickable state: reboots straight
        // into the RP2350's USB mass-storage bootloader, ready for a
        // new .uf2 to be dragged onto it. reset_usb_boot() never
        // returns, so there's no "after" state to show -- the
        // feedback message has to be drawn and actually visible
        // BEFORE calling it, not something the user navigates away
        // from afterward.
        drawBox();
        d_->txtColor(0xFFFF, 0x0000);
        d_->txtSetCursor(MenuFrame::X + 20, MenuFrame::Y + 20);
        d_->txtWrite("Entering Bootsel mode...");
        d_->txtSetCursor(MenuFrame::X + 20, MenuFrame::Y + 20 + MenuFrame::RowPitch);
        d_->txtWrite("Drag a new .uf2 onto the");
        d_->txtSetCursor(MenuFrame::X + 20, MenuFrame::Y + 20 + 2 * MenuFrame::RowPitch);
        d_->txtWrite("drive that appears.");
#ifdef DISPLAY_7INCH
        // handleButton()'s own end-of-function overlay show (see its
        // own comment) never runs for this path -- reset_usb_boot()
        // below never returns, so this message needs to be made
        // visible right here, or it would sit drawn-but-invisible on
        // the menu layer while the PIP overlay kept showing whatever
        // was on screen before this button press.
        d_->endOverlayDraw();
        d_->showPipOverlay(MenuFrame::X, MenuFrame::MenuTop, MenuFrame::W, MenuFrame::MenuH);
#endif
#if HIPI_DEV_SCREENDUMPS
        devScreendump("Bootsel message");      // last chance -- we reboot next
#endif
        sleep_ms(1500);  // long enough to actually read before reboot
        reset_usb_boot(0, 0);
    }




    static int closestBrightnessIndex(std::uint8_t level) {
        int best = 0;
        int bestDiff = 256;
        for (int i = 0; i < kBrightnessCount; ++i) {
            int diff = level - static_cast<int>(kBrightnessLevels[i]);
            if (diff < 0) diff = -diff;
            if (diff < bestDiff) { bestDiff = diff; best = i; }
        }
        return best;
    }


    // Case-insensitive check whether filename ends with the given extension
    // (extension should include the leading dot, e.g. ".dat").
    static bool hasExtension(const char* filename, const char* extension) {
        const std::size_t nameLen = std::strlen(filename);
        const std::size_t extLen  = std::strlen(extension);
        if (extLen > nameLen) return false;
        const char* tail = filename + (nameLen - extLen);
        for (std::size_t i = 0; i < extLen; ++i) {
            if (std::tolower(static_cast<unsigned char>(tail[i])) !=
                std::tolower(static_cast<unsigned char>(extension[i]))) {
                return false;
            }
        }
        return true;
    }

    // How many rows fit in the box at MenuFrame::RowPitch, minus top/bottom
    // margin. Was a plain "10" sized for the old, tighter 24px pitch.
    // Shared by every scrollable list in this file (FilePicker,
    // DeviceList) -- purely geometric, nothing file-specific about it
    // despite the name.
    static constexpr int kMaxFilesShown =
        (MenuFrame::H - 40) / MenuFrame::RowPitch;

    void openFilePicker() {
        // The drive whose Tape view was shown last (the first drive until
        // then) -- with several drives, each view picks its own file
        if (CDrive* d = plotterview_drive()) targetDrive_ = d;
        setMediaEjected(true);              // cassette out while choosing
        files_.clear();
        files_.push_back("");               // first row: "No media" (no file selected)
        DIR dir;
        if (f_opendir(&dir, HIPI_DIR_LIF) == FR_OK) {      // cassettes live in lif/
            FILINFO info;
            while (f_readdir(&dir, &info) == FR_OK && info.fname[0] != 0) {
                if (!(info.fattrib & AM_DIR) && hasExtension(info.fname, ".dat")) {
                    files_.push_back(info.fname);
                }
            }
            f_closedir(&dir);
        }
        state_ = State::FilePicker;
        // Highlight the drive's current file, same idea as every other
        // multi-choice menu (Textcolor/Font size/Brightness/Columns) --
        // falls back to index 0 ("No media") if it's not in the list
        // (e.g. deleted or renamed since).
        selected_ = 0;
        const std::string current = targetDrive_ ? targetDrive_->mediaFile() : std::string();
        for (std::size_t i = 0; i < files_.size(); ++i) {
            if (files_[i] == current) { selected_ = static_cast<int>(i); break; }
        }
        // Unlike every other menu here, this list can hold more entries
        // than fit in the box at once (kMaxFilesShown) -- previously
        // always showed just the first kMaxFilesShown files regardless
        // of where the selection actually was, silently hiding it
        // whenever it fell further down the list than that. Scrolls the
        // window so the selected entry is visible from the start now,
        // same as moveFileSelection() keeps it during Up/Down navigation.
        fileScrollOffset_ = 0;
        if (selected_ >= kMaxFilesShown) {
            fileScrollOffset_ = selected_ - kMaxFilesShown + 1;
        }
        title_ = "SELECT CASSETTE";
        drawBox();
        drawFileList();
    }

    // Redraws the currently-scrolled-to window of files_ into the menu
    // box -- shared by openFilePicker() (initial draw) and
    // moveFileSelection() (whenever scrolling is needed after Up/Down).
    // Draws small "more above"/"more below" markers in the box's top-right
    // and bottom-right corners whenever the list has more entries than fit
    // in the visible kMaxFilesShown-row window at the current scroll
    // position. Shared by every scrollable list (drawFileList(),
    // drawDeviceList()) -- clears both corners first each time, since a
    // shorter list scrolled to afterward could otherwise leave a stale
    // arrow from a longer one showing.
    void drawScrollIndicators(int scrollOffset, int totalCount, int visibleRows = kMaxFilesShown) {
        // The marks are glyphs of the menu font, 8*(scale+1) x 16*(scale+1)
        // px (16x32 at TextScale 1) -- the bottom one must end ABOVE the
        // frame's bottom border, or its background cuts a gap in it (it
        // was placed for a ~20 px glyph before).
        constexpr int kGlyphW = 8 * (MenuFrame::TextScale + 1);
        constexpr int kGlyphH = 16 * (MenuFrame::TextScale + 1);
        const int indicatorX = MenuFrame::X + MenuFrame::W - 34;
        const int topY = MenuFrame::Y + 6;
        const int bottomY = MenuFrame::Y + MenuFrame::H - MenuFrame::BorderThickness - kGlyphH - 2;
        d_->fillRect(indicatorX, topY, kGlyphW, kGlyphH, 0x0000);
        d_->fillRect(indicatorX, bottomY, kGlyphW, kGlyphH, 0x0000);
        d_->txtSize(MenuFrame::TextScale);
        d_->txtColor(0xFFFF, 0x0000);
        if (scrollOffset > 0) {
            d_->txtSetCursor(indicatorX, topY);
            d_->txtWrite("^");
        }
        if (scrollOffset + visibleRows < totalCount) {
            d_->txtSetCursor(indicatorX, bottomY);
            d_->txtWrite("v");
        }
    }

    void drawFileList() {
        // See drawColorRow()'s own comment for why this is needed here
        // too -- called on its own during Up/Down navigation (see
        // moveFileSelection()), without drawBox() running again first.
        // Once per call (not per row) is enough -- every row below uses
        // the same scale.
        d_->txtSize(MenuFrame::TextScale);
        for (int row = 0; row < kMaxFilesShown; ++row) {
            const std::size_t fileIndex = static_cast<std::size_t>(fileScrollOffset_ + row);
            const bool hasEntry = fileIndex < files_.size();
            const bool isSelected = hasEntry && (static_cast<int>(fileIndex) == selected_);
            // Clears this row's slot EVERY time, whether or not it ends
            // up holding text -- without this, (a) a shorter replacement
            // string leaves a trailing "tail" of the longer one it
            // replaced still visible, and (b) a row that held an entry
            // last draw but doesn't this time (the list got shorter, or
            // scrolled past its own end) keeps showing that old entry
            // forever, since nothing ever overwrote it.
            _clearRowBackground(row, isSelected ? MenuFrame::Yellow : 0x0000);
            if (!hasEntry) continue;
            // drawRow()'s own highlight check compares against selected_
            // directly (an absolute files_ index), but its own cursor
            // positioning uses its "index" argument for the row's Y
            // position -- pass the SCREEN row here, but fake selected_
            // temporarily isn't needed since drawRow() takes the label
            // separately; just need the highlight test itself to use the
            // right (absolute) index. Simplest: reimplement the same two
            // lines drawRow() itself uses, with row for position and
            // fileIndex for the highlight comparison.
            d_->txtSetCursor(MenuFrame::X + 20, MenuFrame::Y + 20 + row * MenuFrame::RowPitch);
            d_->txtColor(isSelected ? 0x0000 : 0xFFFF, isSelected ? MenuFrame::Yellow : 0x0000);
            d_->txtWrite(fileLabel(fileIndex));
        }
        drawScrollIndicators(fileScrollOffset_, static_cast<int>(files_.size()));
    }

    // Up/Down navigation for State::FilePicker specifically -- moveSelection()
    // (used by every other, always-fits-on-screen menu) only ever redraws
    // the two rows whose highlight changed, which isn't enough here: when
    // the selection moves past the edge of the currently-visible window,
    // the window itself has to scroll, and every visible row's actual
    // file content (not just which one is highlighted) changes at once.
    void moveFileSelection(int delta) {
        const int count = static_cast<int>(files_.size());
        if (count == 0) return;
        selected_ = (selected_ + delta + count) % count;
        if (selected_ < fileScrollOffset_) {
            fileScrollOffset_ = selected_;
        } else if (selected_ >= fileScrollOffset_ + kMaxFilesShown) {
            fileScrollOffset_ = selected_ - kMaxFilesShown + 1;
        }
        drawFileList();
    }

    // Shows every device in the HP-IL chain (see the `devices` global from
    // hipi.cpp) with its current enabled/disabled status. Labels are built
    // fresh each time (name() + state), since device count/state isn't
    // known at compile time like the other menus' fixed label arrays.
    void openDeviceList() {
        deviceLabels_.clear();
        for (CDevice* dev : devices) {
            deviceLabels_.push_back(deviceLabel(dev));
        }
        state_ = State::DeviceList;
        selected_ = 0;
        deviceScrollOffset_ = 0;
        title_ = "DEVICES / ON-OFF";
        drawBox();
        drawDeviceList();
    }

    // Drives show their WANTED state: a drive waiting to switch off
    // (CDrive::requestEnabled()) already shows [OFF]
    static bool deviceWantedOn(CDevice* dev) {
        if (dev->type() == DRIVE) return static_cast<CDrive*>(dev)->wantedEnabled();
        return dev->enabled();
    }
    static std::string deviceLabel(CDevice* dev) {
        return std::string(dev->name()) + (deviceWantedOn(dev) ? " [ON]" : " [OFF]");
    }

    // Redraws the currently-scrolled-to window of deviceLabels_ into the
    // menu box -- shared by openDeviceList() (initial draw) and
    // moveDeviceSelection() (whenever scrolling is needed after Up/Down).
    // Mirrors drawFileList() exactly -- see its own comment for why this
    // reimplements drawRow()'s own two lines rather than calling it
    // directly (screen row for position, absolute index for the
    // highlight comparison).
    void drawDeviceList() {
        d_->txtSize(MenuFrame::TextScale);
        for (int row = 0; row < kMaxFilesShown; ++row) {
            const std::size_t devIndex = static_cast<std::size_t>(deviceScrollOffset_ + row);
            const bool hasEntry = devIndex < deviceLabels_.size();
            const bool isSelected = hasEntry && (static_cast<int>(devIndex) == selected_);
            // See drawFileList()'s own comment on _clearRowBackground()
            // for why this always runs, entry or not.
            _clearRowBackground(row, isSelected ? MenuFrame::Yellow : 0x0000);
            if (!hasEntry) continue;
            d_->txtSetCursor(MenuFrame::X + 20, MenuFrame::Y + 20 + row * MenuFrame::RowPitch);
            d_->txtColor(isSelected ? 0x0000 : 0xFFFF, isSelected ? MenuFrame::Yellow : 0x0000);
            d_->txtWrite(deviceLabels_[devIndex].c_str());
        }
        drawScrollIndicators(deviceScrollOffset_, static_cast<int>(deviceLabels_.size()));
    }

    // Up/Down navigation for State::DeviceList specifically -- mirrors
    // moveFileSelection() exactly; see its own comment for why the
    // generic moveSelection() (every other, always-fits-on-screen menu)
    // isn't enough once there are more devices than fit in the box at
    // once.
    void moveDeviceSelection(int delta) {
        const int count = static_cast<int>(deviceLabels_.size());
        if (count == 0) return;
        selected_ = (selected_ + delta + count) % count;
        if (selected_ < deviceScrollOffset_) {
            deviceScrollOffset_ = selected_;
        } else if (selected_ >= deviceScrollOffset_ + kMaxFilesShown) {
            deviceScrollOffset_ = selected_ - kMaxFilesShown + 1;
        }
        drawDeviceList();
    }

    // ── Devices submenu: Enable/disable, Change order ────────────────────


    // ── Devices -> Change order ──────────────────────────────────────────
    // The loop order of the devices. Up/Down + OK picks one ("moving",
    // shown between > <), Up/Down then moves it, OK keeps the new order
    // (hipi_applyDeviceOrder(): used at once, saved to CONFIG.TXT), X puts
    // it back. X when nothing is picked returns to the Devices menu.
    void openDeviceOrder() {
        orderList_ = devices;
        orderMoving_ = false;
        state_ = State::DeviceOrder;
        selected_ = 0;
        deviceScrollOffset_ = 0;
        title_ = "DEVICES / ORDER";
        drawBox();
        drawDeviceOrder();
    }

    void keepOrderSelectionVisible() {
        if (selected_ < deviceScrollOffset_) {
            deviceScrollOffset_ = selected_;
        } else if (selected_ >= deviceScrollOffset_ + kMaxFilesShown) {
            deviceScrollOffset_ = selected_ - kMaxFilesShown + 1;
        }
    }

    void orderMove(int delta) {
        const int count = static_cast<int>(orderList_.size());
        if (count == 0) return;
        if (!orderMoving_) {
            selected_ = (selected_ + delta + count) % count;
        } else {
            const int to = selected_ + delta;
            if (to < 0 || to >= count) return;          // no wrap while moving
            std::swap(orderList_[static_cast<std::size_t>(selected_)],
                      orderList_[static_cast<std::size_t>(to)]);
            selected_ = to;
        }
        keepOrderSelectionVisible();
        drawDeviceOrder();
    }

    void drawDeviceOrder() {
        d_->txtSize(MenuFrame::TextScale);
        for (int row = 0; row < kMaxFilesShown; ++row) {
            const std::size_t i = static_cast<std::size_t>(deviceScrollOffset_ + row);
            const bool hasEntry = i < orderList_.size();
            const bool isSelected = hasEntry && static_cast<int>(i) == selected_;
            _clearRowBackground(row, isSelected ? MenuFrame::Yellow : 0x0000);
            if (!hasEntry) continue;
            char label[40];
            std::snprintf(label, sizeof(label), (isSelected && orderMoving_) ? "%u > %s <" : "%u  %s",
                          static_cast<unsigned>(i + 1), orderList_[i]->name());
            d_->txtSetCursor(MenuFrame::X + 20, MenuFrame::Y + 20 + row * MenuFrame::RowPitch);
            d_->txtColor(isSelected ? 0x0000 : 0xFFFF, isSelected ? MenuFrame::Yellow : 0x0000);
            d_->txtWrite(label);
        }
        drawScrollIndicators(deviceScrollOffset_, static_cast<int>(orderList_.size()));
    }

    void toggleDevice(int index) {
        if (index < 0 || index >= static_cast<int>(devices.size())) return;
        CDevice* dev = devices[index];
        if (dev->type() == DRIVE) {
            // Switching a drive off is deferred until it's idle -- pulling
            // it out of the loop mid-transfer hangs HP-IL
            CDrive* drive = static_cast<CDrive*>(dev);
            // Same as the power switch: OFF, or back to its last position
            // (STANDBY or ON)
            drive->setPowerMode(drive->wantedEnabled() ? DrivePower::Off
                                                       : drive->lastOnMode());
        } else {
            dev->toggleEnabled();
        }
        deviceLabels_[static_cast<std::size_t>(index)] = deviceLabel(dev);
        // Only actually redraw if this device's own row is within the
        // currently-visible scrolled window -- toggleDevice() is only
        // ever called for the currently-selected device (see
        // handleButton()'s own State::DeviceList case), which
        // moveDeviceSelection() already keeps scrolled into view, so
        // this is really just a safety check rather than a normal case.
        const int row = index - deviceScrollOffset_;
        if (row >= 0 && row < kMaxFilesShown) {
            d_->txtSize(MenuFrame::TextScale);
            // [ON] and [OFF] are different lengths -- without clearing
            // first, toggling OFF->ON leaves a trailing character from
            // "[OFF]" still showing past the end of the now-shorter
            // "[ON]".
            _clearRowBackground(row, MenuFrame::Yellow);
            d_->txtSetCursor(MenuFrame::X + 20, MenuFrame::Y + 20 + row * MenuFrame::RowPitch);
            d_->txtColor(0x0000, MenuFrame::Yellow);  // selected row's own highlight colors
            d_->txtWrite(deviceLabels_[static_cast<std::size_t>(index)].c_str());
        }
        if (onDeviceToggled_) onDeviceToggled_(dev->name(), deviceWantedOn(dev));
    }

    void openConfirmFile(const std::string& filename) {
        pendingFile_ = filename;
        state_ = State::ConfirmFile;
        title_ = "SELECT CASSETTE";
        drawBox();
        drawConfirmText();
    }

    void drawConfirmText() {
        d_->txtColor(0xFFFF, 0x0000);
        d_->txtSetCursor(MenuFrame::X + 20, MenuFrame::Y + 20);
        d_->txtWrite(pendingFile_.empty() ? "Remove cassette?" : "Open file?");
        d_->txtSetCursor(MenuFrame::X + 20, MenuFrame::Y + 20 + MenuFrame::RowPitch);
        d_->txtWrite(pendingFile_.empty() ? kNoMediaLabel : pendingFile_.c_str());
        d_->txtSetCursor(MenuFrame::X + 20, MenuFrame::Y + 20 + 2 * MenuFrame::RowPitch);
        d_->txtWrite("OK = yes    X = cancel");
        if (!pendingFile_.empty()) {
            d_->txtSetCursor(MenuFrame::X + 20, MenuFrame::Y + 20 + 3 * MenuFrame::RowPitch);
            d_->txtWrite("v  = show contents");
        }
    }

    // ── LIF contents (volume header + directory) of pendingFile_ ─────────
    // Smaller font than the other menus (8x16, 20 px rows) so a directory
    // listing fits: kLifInfoRows lines of up to ~65 characters.
    static constexpr int kLifInfoRows  = (MenuFrame::H - 40) / 20;
    static constexpr int kLifInfoPitch = 20;

    void openLifInfo() {
        lifLines_.clear();
        if (usbMscModeActive()) {
            lifLines_.push_back("SD card in use by PC");
        } else {
            lifDescribe((std::string(HIPI_DIR_LIF "/") + pendingFile_).c_str(), lifLines_);
        }
        lifScroll_ = 0;
        state_ = State::LifInfo;
        title_ = "CASSETTE CONTENTS";
        drawBox();
        drawLifInfo();
    }

    void scrollLifInfo(int delta) {
        const int maxOffset = std::max(0, static_cast<int>(lifLines_.size()) - kLifInfoRows);
        const int next = std::clamp(lifScroll_ + delta, 0, maxOffset);
        if (next == lifScroll_) return;
        lifScroll_ = next;
        drawLifInfo();
    }

    void drawLifInfo() {
        d_->selectBuiltinFont();
        d_->txtSize(0);
        for (int row = 0; row < kLifInfoRows; ++row) {
            const int y = MenuFrame::Y + 20 + row * kLifInfoPitch;
            d_->fillRect(MenuFrame::X + 20, y, MenuFrame::W - 60, kLifInfoPitch, 0x0000);
            const std::size_t i = static_cast<std::size_t>(lifScroll_ + row);
            if (i >= lifLines_.size()) continue;
            // fillRect() changes the foreground colour -- (re)set the text
            // colour after it, or the text is drawn black on black
            d_->txtColor(0xFFFF, 0x0000);
            d_->txtSetCursor(MenuFrame::X + 20, y + 2);
            d_->txtWrite(lifLines_[i].c_str());
        }
        // Scroll marks, in the same place as the other lists'
        drawScrollIndicators(lifScroll_, static_cast<int>(lifLines_.size()), kLifInfoRows);
    }

    // Back to the file list with the file just looked at still selected
    void returnToFilePicker() {
        const std::string keep = pendingFile_;
        openFilePicker();
        for (std::size_t i = 0; i < files_.size(); ++i) {
            if (files_[i] == keep) {
                selected_ = static_cast<int>(i);
                if (selected_ < fileScrollOffset_ || selected_ >= fileScrollOffset_ + kMaxFilesShown) {
                    fileScrollOffset_ = std::max(0, selected_ - kMaxFilesShown + 1);
                }
                drawFileList();
                break;
            }
        }
    }

    // ════════════════════════════════════════════════════════════════════
    // The menus. One kind of list for all of them (State::List): rows that
    //  - open another menu or dialog ("Settings >"),
    //  - do something ("Clear plotter"),
    //  - show a setting with its value ("Plot view\tFit"): OK steps to
    //    the next value, which is used and saved at once.
    // The same order everywhere: where to go, what to do, settings, more.
    //
    // OK opens the menu of the view that's showing (its context menu); its
    // last row, "More >", leads to the HIPI menu: Go to view, Settings,
    // System. Every menu has its title in the frame's top band.
    // ════════════════════════════════════════════════════════════════════
    struct Item {
        std::function<std::string()> label;
        std::function<void()> ok;
    };
    std::vector<Item> items_;
    std::function<void()> back_;             // X (nullptr: close the menu)
    int itemScroll_ = 0;
    std::string title_;                      // the frame's top band
    std::uint16_t rowFg_ = 0xFFFF;           // text colour of the row being drawn (writeRowLabel())
    const char* version_ = "";

    static Item item(const std::string& text, std::function<void()> ok) {
        return Item{ [text] { return text; }, std::move(ok) };
    }
    // A setting row: "name" left, the current value right-aligned
    static Item value(const std::string& name, std::function<std::string()> val, std::function<void()> next) {
        return Item{ [name, val] { return name + "\t" + val(); }, std::move(next) };
    }

    void openList(const std::string& title, std::vector<Item> items, std::function<void()> back, int sel = 0) {
        menuOpening();
        state_ = State::List;
        title_ = title;
        items_ = std::move(items);
        back_ = std::move(back);
        selected_ = std::max(0, std::min(sel, static_cast<int>(items_.size()) - 1));
        itemScroll_ = 0;
        keepItemVisible();
        drawBox();
        drawItems();
    }
    void keepItemVisible() {
        if (selected_ < itemScroll_) itemScroll_ = selected_;
        else if (selected_ >= itemScroll_ + kMaxFilesShown) itemScroll_ = selected_ - kMaxFilesShown + 1;
    }
    void moveItemSelection(int delta) {
        const int n = static_cast<int>(items_.size());
        if (n == 0) return;
        selected_ = (selected_ + delta + n) % n;
        keepItemVisible();
        drawItems();
    }
    void drawItems() {
        // A setting row's action may have changed the text screen behind the
        // menu (Font size, Columns...): Screen then leaves the shared text
        // registers (font, scale, active window) set for itself -- set them
        // back for the menu, or its rows come out in the wrong font/clipped
        d_->setActiveWindow(0, 0, SCREEN_MAX_X - 1, SCREEN_MAX_Y - 1);
        d_->selectBuiltinFont();
        d_->txtSize(MenuFrame::TextScale);
        std::vector<std::string> labels;
        for (const Item& it : items_) labels.push_back(it.label());
        drawListRows(labels, itemScroll_);
    }
    // After a setting changed: the rows again (values), same place
    void refreshItems() { drawItems(); }

    // Things every menu needs when it opens from a closed state
    void menuOpening() {
        if (state_ != State::Closed) return;
#ifndef DISPLAY_7INCH
        screen_.suspend();          // (5") stops the text drawing over the menu
#endif
        d_->setTextCursorVisible(false, false);
    }

    // Runs a view/panel action on the main canvas, then closes the menu
    template <typename F> void doAndClose(F&& fn) {
        withMainCanvas(std::forward<F>(fn));
        close();
    }

    void screendumpAction() {
        // The menu is a PIP overlay (7"), not part of the panel's image.
        // Saving takes a few seconds and shows its own messages, so it is
        // not done here, inside the menu's button handling: close the menu
        // first, then let boardui_poll() take the dump from the main loop
        // (the same path as the HP-41 escape sequence), with the menu gone
        // and the PIP overlay free for the "Saving" / "Saved <file>" box.
        doAndClose([] { screendump_request(false); });
    }

    // ── Values of the settings ──────────────────────────────────────────
    int colorIndex() const {
        for (int i = 0; i < kColorCount; ++i) if (kColors[i] == config.textColor()) return i;
        return 0;
    }
    int brightnessIndex() const {
        int best = 0;
        for (int i = 0; i < kBrightnessCount; ++i)
            if (std::abs(kBrightnessLevels[i] - config.brightness()) < std::abs(kBrightnessLevels[best] - config.brightness())) best = i;
        return best;
    }
    int columnsIndex() const {
        for (int i = 0; i < kColumnsCount; ++i) if (kColumnsValues[i] == config.columns()) return i;
        return 0;
    }
    int traceIndex() const { return bExtTrace ? 2 : (bTrace ? 1 : 0); }
    Item fontSizeItem() {
        return value("Font size", [] { return std::to_string(config.fontSize()); },
                     [this] { applyFontSize((config.fontSize() + 1) % kFontSizeCount); refreshItems(); });
    }
    Item plotViewItem() {
        return value("Plot view", [] { return std::string(plotterview_plotFit() ? "Fit" : "Page"); },
                     [this] {
                         const bool fit = !plotterview_plotFit();
                         config.setPlotFit(fit);
                         withMainCanvas([&] { plotterview_setPlotFit(fit); });
                         refreshItems();
                     });
    }
    Item paperItem() {
        return value("Paper", [] { return std::string(plotterview_paperBlack() ? "Black" : "White"); },
                     [this] {
                         const bool black = !plotterview_paperBlack();
                         config.setPlotPaperBlack(black);
                         withMainCanvas([&] { plotterview_setPaperBlack(black); });
                         refreshItems();
                     });
    }
    // "Drive at start" as in CONFIG.TXT: OFF if the (first) drive is
    // switched off at start (disabled_devices), else drive_standby
    static const char* driveAtStartName() {
        for (CDevice* dev : devices)
            if (dev->type() == DRIVE && !config.isDeviceEnabled(dev->name())) return "OFF";
        return config.driveStandby() ? "STANDBY" : "ON";
    }
        static const char* powerName(DrivePower p) {
        return p == DrivePower::On ? "ON" : p == DrivePower::Standby ? "STANDBY" : "OFF";
    }

    // ── The context menu: the view that's showing ───────────────────────
    // `sel`: the row to highlight (back from a submenu)
    void openContextMenu(int sel = 0) {
        const DisplayOutput out = plotterview_output();
        std::vector<Item> m;
        std::string title;
        if (out == DisplayOutput::Plotter) {
            title = "PLOTTER";
            m.push_back(item("Clear plotter", [this] { doAndClose([] { plotterview_clearPlotter(); }); }));
            if (kHasScreendump) m.push_back(item("Screendump", [this] { screendumpAction(); }));
            m.push_back(plotViewItem());
            m.push_back(paperItem());
        } else if (out == DisplayOutput::Tape) {
            CDrive* drv = plotterview_drive();
            title = drv ? std::string("TAPE ") + drv->name() : std::string("TAPE");
            m.push_back(item("Select cassette >", [this] {
                pickerFromTape_ = true;
                openFilePicker();
            }));
            if (drv) {
                m.push_back(item("Rewind", [this, drv] { drv->manualRewind(); close(); }));
                m.push_back(value("Power", [drv] { return std::string(powerName(drv->powerMode())); },
                                  [this, drv] {
                                      const DrivePower p = drv->powerMode();
                                      drv->setPowerMode(p == DrivePower::Off ? DrivePower::Standby
                                                      : p == DrivePower::Standby ? DrivePower::On : DrivePower::Off);
                                      refreshItems();
                                  }));
            }
            if (kHasScreendump) m.push_back(item("Screendump", [this] { screendumpAction(); }));
        } else if (out == DisplayOutput::Analyzer) {
            title = "ANALYZER";
            m.push_back(item("Leave view", [this] { doAndClose([] { plotterview_leaveAnalyzer(); }); }));
            m.push_back(item("Clear", [this] { analyzer_clear(); close(); }));
            m.push_back(Item{ [] { return std::string(analyzer_logging() ? "Logging (on) >" : "Logging >"); },
                              [this] { openAnalyzerLogMenu(); } });
            m.push_back(value("Mode", [] { return std::string(analyzer_mode() == AnalyzerMode::Overview ? "Overview" : "Detailed"); },
                              [this] {
                                  analyzer_setMode(analyzer_mode() == AnalyzerMode::Overview ? AnalyzerMode::Detailed
                                                                                             : AnalyzerMode::Overview);
                                  refreshItems();
                              }));
            m.push_back(value("Time", [] { return std::string(analyzer_relativeTime() ? "Relative" : "Since start"); },
                              [this] { analyzer_setRelativeTime(!analyzer_relativeTime()); refreshItems(); }));
            m.push_back(value("Idle frames", [] { return std::string(analyzer_showIdle() ? "Shown" : "Hidden"); },
                              [this] { analyzer_setShowIdle(!analyzer_showIdle()); refreshItems(); }));
        } else if (out == DisplayOutput::LoopMap || out == DisplayOutput::Signals) {
            const bool map = out == DisplayOutput::LoopMap;
            title = map ? "LOOP MAP" : "HP-IL SIGNALS";
            m.push_back(item("Leave view", [this, map] {
                doAndClose([map] { if (map) plotterview_leaveLoopMap(); else plotterview_leaveSignals(); });
            }));
            if (kHasScreendump) m.push_back(item("Screendump", [this] { screendumpAction(); }));
            if (!map) {
                m.push_back(value("Bit cells", [] { return std::string(hpil_diag_bitCellsName(hpil_diag_bitCells())); },
                                  [this] {
                                      const auto next = static_cast<BitCells>(
                                          (static_cast<int>(hpil_diag_bitCells()) + 1) % 3);
                                      config.setSignalCells(static_cast<std::uint8_t>(next));
                                      withMainCanvas([&] { hpil_diag_setBitCells(next); });
                                      refreshItems();
                                  }));
            }
        } else {
            title = "DISPLAY";
            m.push_back(item("Clear screen", [this] { doAndClose([this] { screen_.clear(); }); }));
            if (kHasScreendump) m.push_back(item("Screendump", [this] { screendumpAction(); }));
            m.push_back(fontSizeItem());
        }
        m.push_back(item("More >", [this] { openHipiMenu(); }));
        openList(title, std::move(m), nullptr, sel);
    }

    // ── More: the HIPI menu ─────────────────────────────────────────────
    void openHipiMenu(int sel = 0) {
        openList("HIPI", {
            item("Go to view >", [this] { openViewsMenu(); }),
            item("Settings >",   [this] { openSettingsMenu(); }),
            item("System >",     [this] { openSystemMenu(); }),
        }, [this] { openContextMenu(); }, sel);
    }

    void openViewsMenu() {
        std::vector<Item> m;
        int sel = 0;
        for (CDevice* dev : plotterview_viewDevices()) {
            if (dev == plotterview_viewDevice() && plotterview_output() != DisplayOutput::Analyzer &&
                plotterview_output() != DisplayOutput::LoopMap && plotterview_output() != DisplayOutput::Signals)
                sel = static_cast<int>(m.size());
            m.push_back(item(plotterview_viewTitle(dev), [this, dev] { doAndClose([dev] { plotterview_showDevice(dev); }); }));
        }
        m.push_back(item("Analyzer",      [this] { doAndClose([] { plotterview_showAnalyzer(); }); }));
        m.push_back(item("Loop map",      [this] { doAndClose([] { plotterview_showLoopMap(); }); }));
        m.push_back(item("HP-IL signals", [this] { doAndClose([] { plotterview_showSignals(); }); }));
        openList("GO TO VIEW", std::move(m), [this] { openHipiMenu(0); }, sel);
    }

    // ── Settings ────────────────────────────────────────────────────────
    void openSettingsMenu(int sel = 0) {
        openList("SETTINGS", {
            item("Screen >",       [this] { openScreenSettings(); }),
            item("Screen saver >", [this] { openSaverSettings(); }),
            item("Devices >",      [this] { openDevicesMenu(); }),
            item("PC link >",      [this] { openPcLinkSettings(); }),
        }, [this] { openHipiMenu(1); }, sel);
    }

    void openScreenSettings() {
        openList("SETTINGS / SCREEN", {
            value("Text colour", [this] { return std::string(kColorLabels[colorIndex()]); },
                  [this] { withMainCanvas([&] { applyColor((colorIndex() + 1) % kColorCount); }); refreshItems(); }),
            fontSizeItem(),
            value("Columns", [this] { return std::string(kColumnsLabels[columnsIndex()]); },
                  [this] { applyColumns((columnsIndex() + 1) % kColumnsCount); refreshItems(); }),
            value("Brightness", [this] { return std::string(kBrightnessLabels[brightnessIndex()]); },
                  [this] { applyBrightness((brightnessIndex() + 1) % kBrightnessCount); refreshItems(); }),
            plotViewItem(),
            paperItem(),
#if defined(DISPLAY_7INCH) && HIPI_MIRROR_PORT && HIPI_MIRROR_TRANSPORT
            // The display mirror for tools/hipiview (USB CDC3)
            value("Mirror to PC", [] { return std::string(displayMirror_enabled() ? "On" : "Off"); },
                  [this] {
                      const bool on = !displayMirror_enabled();
                      config.setDisplayMirror(on);
                      displayMirror_setEnabled(on);
                      refreshItems();
                  }),
#endif
        }, [this] { openSettingsMenu(0); });
    }

    void openSaverSettings() {
        static const char* kModes[] = { "Dim", "Clock", "Off", "HP-IL rain", "Goose", "Random" };
        auto name = [](std::uint8_t m) { return std::string(kModes[m < hipi::Config::kSaverModes ? m : 0]); };
        openList("SETTINGS / SCREEN SAVER", {
            value("Mode", [name] { return name(config.saverMode()); }, [this] {
                // Dim -> Clock -> HP-IL rain -> Goose -> Random -> Off -> Dim
                static const std::uint8_t kNext[] = { hipi::Config::SaverClock, hipi::Config::SaverRain,
                                                      hipi::Config::SaverDim, hipi::Config::SaverGoose,
                                                      hipi::Config::SaverRandom, hipi::Config::SaverOff };
                const std::uint8_t m = config.saverMode();
                config.setSaverMode(kNext[m < hipi::Config::kSaverModes ? m : 0]);
                refreshItems();
            }),
            value("No clock", [name] { return name(config.clockFallback()); }, [this] {
                const std::uint8_t f = config.clockFallback();
                config.setClockFallback(f == hipi::Config::SaverRain   ? hipi::Config::SaverGoose
                                      : f == hipi::Config::SaverGoose  ? hipi::Config::SaverRandom
                                      : f == hipi::Config::SaverRandom ? hipi::Config::SaverDim
                                                                       : hipi::Config::SaverRain);
                refreshItems();
            }),
            value("Style", [] { return std::string(config.clockStyle() == 1 ? "LCD" : "Dark"); },
                  [this] { config.setClockStyle(config.clockStyle() == 1 ? 0 : 1); refreshItems(); }),
            value("Date", [] { return std::string(config.clockUs() ? "US" : "EU"); },
                  [this] { config.setClockUs(!config.clockUs()); refreshItems(); }),
            value("Time", [] { return std::string(config.clock12h() ? "12 h" : "24 h"); },
                  [this] { config.setClock12h(!config.clock12h()); refreshItems(); }),
            item("Show now", [this] { backlight_showClockNow(); close(); }),
        }, [this] { openSettingsMenu(1); });
    }

    void openDevicesMenu(int sel = 0) {
        openList("SETTINGS / DEVICES", {
            item("Enable/disable >", [this] { openDeviceList(); }),
            item("Change order >",   [this] { openDeviceOrder(); }),
            // The cassette drives' power switch at the next start: ON ->
            // STANDBY -> OFF (= switched off, like Enable/disable) -> ON
            value("Drive at start", [this] { return std::string(driveAtStartName()); }, [this] {
                const std::string now = driveAtStartName();
                const bool on = now == "OFF";                // OFF -> ON
                const bool standby = now == "ON";            // ON -> STANDBY
                for (CDevice* dev : devices)
                    if (dev->type() == DRIVE) config.setDeviceEnabled(dev->name(), now != "STANDBY");
                config.setDriveStandby(standby && !on);
                refreshItems();
            }),
        }, [this] { openSettingsMenu(2); }, sel);
    }

    void openPcLinkSettings() {
        openList("SETTINGS / PC LINK", {
            value("CON loop", [] { return std::string(config.conInternal() ? "Internal" : "Cable"); },
                  [this] { config.setConInternal(!config.conInternal()); refreshItems(); }),
            value("Trace", [this] { return std::string(kTraceLabels[traceIndex()]); },
                  [this] { applyTrace((traceIndex() + 1) % kTraceCount); refreshItems(); }),
        }, [this] { openSettingsMenu(3); });
    }

    // ── System ──────────────────────────────────────────────────────────
    void openSystemMenu(int sel = 0) {
        openList("SYSTEM", {
            Item{ [this] { return std::string((usbMscModeActive() || devForceDisconnectLabel_) ? "Disconnect from PC" : "Connect to PC"); },
                  [this] { toggleConnectToPc(); } },
            item("Loopback test >", [this] { openLoopbackConfirm(); }),
            item("Scan I2C >",      [this] { runI2CScan(); }),
            item("Character table >", [this] { doAndClose([] { plotterview_showCharTable(); }); }),
            item("About >",         [this] { openAbout(); }),
            item("Bootsel mode",    [this] { enterBootselMode(); }),
        }, [this] { openHipiMenu(2); }, sel);
    }

    void openAbout() {
        char b[48];
        std::vector<Item> m;
        auto line = [&](const std::string& s) { m.push_back(item(s, [] {})); };
        line(std::string("HIPI ") + version_);
        line("HP-IL Pico Interface");
#ifdef DISPLAY_7INCH
        line("7\" display, Pico 2");
#else
        line("5\" display, Pico 2");
#endif
        std::snprintf(b, sizeof(b), "Built %s", __DATE__);
        line(b);
        openList("SYSTEM / ABOUT", std::move(m), [this] { openSystemMenu(4); });
    }

    // "Connect to PC" / "Disconnect from PC" -- the SD card as a USB drive
    // (usb_msc.h). Shows the result for a moment, then the System menu again.
    void toggleConnectToPc() {
        const bool wasActive = usbMscModeActive();
#if HIPI_DEV_SCREENDUMPS
        if (!wasActive) devDumpConnectScreens();
#endif
        const bool nowActive = wasActive ? (exitUsbMscMode(), false) : enterUsbMscMode();
        title_ = "SYSTEM";
        drawBox();
        d_->txtColor(0xFFFF, 0x0000);
        d_->txtSetCursor(MenuFrame::X + 20, MenuFrame::Y + 20);
        if (wasActive) {
            d_->txtWrite("Disconnected from PC.");
            d_->txtSetCursor(MenuFrame::X + 20, MenuFrame::Y + 20 + MenuFrame::RowPitch);
            d_->txtWrite("HP-IL drive access resumed.");
        } else if (nowActive) {
            drawConnectedText();
        } else {
            d_->txtWrite("Couldn't connect -- no SD card?");
        }
        sleep_ms(1500);
        openSystemMenu(0);
    }

    void close() {
        // Brightness live-preview (see handleButton()'s own
        // BrightnessMenu case) needs restoring here too, not just on
        // the menu's own X/cancel -- close() is also reached via the
        // global Shift+Ok "exit entirely" shortcut, which can fire
        // while BrightnessMenu is open and mid-preview, bypassing that
        // X handling entirely.
        state_ = State::Closed;
        pickerFromTape_ = false;
        setMediaEjected(false);             // cassette back in (new or old file)
        boardui_onMenuClosed();  // shortens the button strip's auto-hide
                                  // countdown -- see its definition in
                                  // boardui.cpp for why
#ifdef DISPLAY_7INCH
        // The menu was only ever a PIP overlay on top of whatever's
        // actually showing on the main window (text, a plotter view, a
        // splash screen) -- none of that was ever hidden or paused, just
        // visually covered. Disabling the overlay uncovers it again
        // immediately, correct regardless of what it turns out to be --
        // none of the splash/plotter special-casing the 5" path below
        // still needs is necessary here.
        // Exception: if this menu action just switched view, the switch
        // splash has taken over the PIP overlay -- leave it
        // up; plotterview_poll() removes it when its timer expires.
        if (!plotterview_isSplashVisible()) d_->hidePipOverlay();
#else
        if (plotterview_isSplashVisible()) {
            // A view switch just happened (this menu action selected a
            // new Display/Plotter output) -- the splash announcing it
            // already covers the whole screen, menu box included, and
            // its own timeout (see plotterview_poll()) reveals the real
            // content shortly. Redrawing here now would just prematurely
            // wipe the splash out early.
            return;
        }
        if (plotterview_output() == DisplayOutput::Plotter ||
            plotterview_output() == DisplayOutput::Analyzer ||
            plotterview_output() == DisplayOutput::LoopMap ||
            plotterview_output() == DisplayOutput::Signals ||
            plotterview_output() == DisplayOutput::CharTable) {
            // Screen stays suspended (it's not what's showing) -- just
            // erase the menu box by redrawing the plot underneath it,
            // instead of screen_.resume()'s HP-41 text redraw below.
            plotterview_redraw();
            return;
        }
        screen_.resume();   // re-enables output and redraws the buffer,
                             // catching up on anything written while the
                             // dialog was open
#endif
    }

    void moveSelection(int delta, int count) {
        if (count == 0) return;
        int old = selected_;
        selected_ = (selected_ + delta + count) % count;
        highlightRow(old, false);
        highlightRow(selected_, true);
    }

    void applyColor(int index) {
        screen_.setColor(kColors[index]);  // persists color_, not just the live register
        if (onColorChanged_) onColorChanged_(kColors[index]);
    }

    void applyFontSize(int index) {
        withMainCanvas([&]{ screen_.setTextSize(static_cast<std::uint8_t>(index)); });
        if (onFontSizeChanged_) onFontSizeChanged_(static_cast<std::uint8_t>(index));
    }

    void applyBrightness(int index) {
        const std::uint8_t level = kBrightnessLevels[index];
        screen_.setBrightness(level);
        if (onBrightnessChanged_) onBrightnessChanged_(level);
    }

    void applyColumns(int index) {
        const std::uint8_t cols = kColumnsValues[index];
        withMainCanvas([&]{ screen_.setColumns(cols); });
        if (onColumnsChanged_) onColumnsChanged_(cols);
    }

    void applyTrace(int index) {
        // 0=Off (both false), 1=On (trace only), 2=Extended (both true).
        bTrace = (index >= 1);
        bExtTrace = (index == 2);
        LOGF("\r\n * Trace: %s", kTraceLabels[index]);
        if (onTraceChanged_) onTraceChanged_(bTrace, bExtTrace);
    }

    void applyFile(const std::string& filename) {
        LOGF("\r\n * Selected file: %s", filename.empty() ? kNoMediaLabel : filename.c_str());
        // The file belongs to the drive (see setTargetDrive())
        if (targetDrive_) targetDrive_->setMediaFile(filename);
    }



    // The frame, with the menu's title (title_) in its top band -- in the
    // HP-41's display font (HIPI_MENU_TITLE_HP41FONT) or the built-in one
    void drawBox() {
        using namespace MenuFrame;
        MenuFrame::draw(d_, X, MenuTop, W, MenuH);
        d_->fillRect(X + BorderThickness, MenuTop + BorderThickness, W - 2 * BorderThickness,
                     TitleH - BorderThickness, Yellow);
        std::string t = title_.empty() ? std::string("HIPI") : title_;
        for (char& c : t) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
#if HIPI_MENU_TITLE_HP41FONT
        drawGlyphText(d_, X + 16, MenuTop + (TitleH - kMenuTitleFont.height) / 2 + 1, kMenuTitleFont,
                      t.c_str(), 0x0000, Yellow);
        d_->selectBuiltinFont();
#else
        d_->txtSize(0);
        d_->txtColor(0x0000, Yellow);
        d_->txtSetCursor(X + 16, MenuTop + 8);
        d_->txtWrite(t.c_str());
#endif
        d_->txtSize(TextScale);
    }

    // Clears the full width of one menu row's own slot to the given
    // background color, BEFORE any text gets written there -- without
    // this, a row that previously held a longer string (or was
    // highlighted, i.e. had a different background) than what's about to
    // be drawn on it leaves a leftover "tail" of the old content/color
    // showing past the end of the new, shorter text. Shared by drawRow()
    // (the common case, one label per row) and drawFileList()/
    // drawDeviceList() (their own scrolled windows, where the very same
    // screen row can go from a long entry to a short one, or from
    // selected to not, on every single Up/Down press).
    //
    // Width stops at MenuFrame::W - 60 rather than the box's own right
    // edge -- leaves the scroll-indicator column (see
    // drawScrollIndicators(), which sits at MenuFrame::W - 34) alone, so
    // clearing a row's own text never also erases the "more above/below"
    // arrows drawn independently of it.
    void _clearRowBackground(int row, std::uint16_t bg) {
        d_->fillRect(MenuFrame::X + 20, MenuFrame::Y + 20 + row * MenuFrame::RowPitch,
                     MenuFrame::W - 60, MenuFrame::RowPitch, bg);
    }

    void drawRow(int index, const char* label) {
        // See drawColorRow()'s own comment for why this is needed here
        // too -- called on its own during Up/Down navigation, without
        // drawBox() running again first. Same reasoning for
        // selectBuiltinFont() as txtSize() right beside it -- Screen's
        // own async HP-IL-driven draws could in principle interleave
        // between this row and drawBox()'s own earlier call (see
        // Screen.cpp's identical comment on selectCustomFont()), so this
        // reasserts it explicitly per row rather than trusting it's
        // still set from MenuFrame::draw().
        d_->txtSize(MenuFrame::TextScale);
        d_->selectBuiltinFont();
        const bool isSelected = (index == selected_);
        _clearRowBackground(index, isSelected ? MenuFrame::Yellow : 0x0000);
        rowFg_ = isSelected ? 0x0000 : 0xFFFF;
        d_->txtColor(rowFg_, isSelected ? MenuFrame::Yellow : 0x0000);
        writeRowLabel(MenuFrame::Y + 20 + index * MenuFrame::RowPitch, label);
    }

    // A row's label. One ending in " >" (opens another menu or dialog) gets
    // its ">" right-aligned in the row, like a submenu arrow.
    void writeRowLabel(int y, const char* label) {
        if (const char* tab = std::strchr(label, '\t')) {
            // A setting: its name left, the value right-aligned between two
            // filled arrows -- drawn, not text, so they don't look like the
            // ">" of a submenu row
            const std::string name(label, static_cast<std::size_t>(tab - label));
            const std::string val(tab + 1);
            d_->txtSetCursor(MenuFrame::X + 20, static_cast<std::uint16_t>(y));
            d_->txtWrite(name.c_str());
            constexpr int kArrowW = 10, kGap = 10;
            const int valW = static_cast<int>(val.size()) * 16;
            const int right = MenuFrame::X + MenuFrame::W - 48;           // right arrow's tip
            int vx = right - kArrowW - kGap - valW;
            vx = std::max(vx, MenuFrame::X + 20 + static_cast<int>(name.size() + 1) * 16 + kArrowW + kGap);
            d_->txtSetCursor(static_cast<std::uint16_t>(vx), static_cast<std::uint16_t>(y));
            d_->txtWrite(val.c_str());
            // The arrows in the text colour of the row (white, or black when selected)
            const std::uint16_t col = rowFg_;
            const int cy = y + 16, h = 9;
            const int lx = vx - kGap;                                      // left arrow's base
            d_->fillTriangle(static_cast<std::int16_t>(lx - kArrowW), static_cast<std::int16_t>(cy),
                             static_cast<std::int16_t>(lx), static_cast<std::int16_t>(cy - h),
                             static_cast<std::int16_t>(lx), static_cast<std::int16_t>(cy + h), col);
            const int rx = vx + valW + kGap;                               // right arrow's base
            d_->fillTriangle(static_cast<std::int16_t>(rx + kArrowW), static_cast<std::int16_t>(cy),
                             static_cast<std::int16_t>(rx), static_cast<std::int16_t>(cy - h),
                             static_cast<std::int16_t>(rx), static_cast<std::int16_t>(cy + h), col);
            return;
        }
        const std::size_t len = std::strlen(label);
        const bool sub = len >= 2 && label[len - 1] == '>' && label[len - 2] == ' ';
        d_->txtSetCursor(MenuFrame::X + 20, static_cast<std::uint16_t>(y));
        if (!sub) {
            d_->txtWrite(label);
            return;
        }
        const std::string text(label, len - 2);
        d_->txtWrite(text.c_str());
        d_->txtSetCursor(MenuFrame::X + MenuFrame::W - 64, static_cast<std::uint16_t>(y));
        d_->txtWrite(">");
    }

    void highlightRow(int index, bool /*selected*/) {
        // drawRow already looks at selected_ to pick the color scheme,
        // so we only need to point it at the right label source for the active state.
        switch (state_) {
            case State::List:
                (void)index;
                drawItems();
                break;
            case State::FilePicker:
                if (index >= 0 && index < static_cast<int>(files_.size()))
                    drawRow(index, fileLabel(static_cast<std::size_t>(index)));
                break;
            case State::DeviceList:
                if (index >= 0 && index < static_cast<int>(deviceLabels_.size()))
                    drawRow(index, deviceLabels_[static_cast<std::size_t>(index)].c_str());
                break;
            case State::AnalyzerLogMenu:
                drawListRows(analyzerLogLabels_, analyzerLogScroll_);
                break;
            default:
                break;
        }
    }

    DisplayDriver* d_;
    Screen& screen_;
    State state_ = State::Closed;
    int selected_ = 0;
    bool lastShifted_ = false;      // Shift state of the last press (auto-repeat)
    bool shiftPending_ = false;   // latched by Button::Shift (any state),
                                    // consumed by the next button press
    std::vector<std::string> files_;
    // See openFilePicker()'s own comment for why these exist -- neither
    // did before.
    int fileScrollOffset_ = 0;
    int deviceScrollOffset_ = 0;
    std::vector<CDevice*> orderList_;     // DeviceOrder: the order being edited
    bool orderMoving_ = false;            // DeviceOrder: a device is picked up
    int orderPickedFrom_ = 0;             // DeviceOrder: where it was picked up
    CDrive* targetDrive_ = nullptr;       // see setTargetDrive()
    std::vector<std::string> lifLines_;   // LifInfo: description lines
    int lifScroll_ = 0;                   // LifInfo: first line shown

    // The file picker's first row is an empty name, shown as "No media":
    // choosing it deselects the drive's file (drive reports no tape).
    static constexpr const char* kNoMediaLabel = "No media";
    const char* fileLabel(std::size_t i) const {
        return files_[i].empty() ? kNoMediaLabel : files_[i].c_str();
    }
    // True while the file picker was opened directly from the Tape view
    // (openFilePickerFromTape()) rather than via Config -- X/OK then close
    // the menu instead of returning to the Config menu. Reset by close().
    bool pickerFromTape_ = false;
    std::string pendingFile_;
    std::vector<std::string> deviceLabels_;
    bool mediaEjected_ = false;

    // Cassette out/in: the Tape view (open lid) and the drive itself
    void setMediaEjected(bool ejected) {
        if (ejected == mediaEjected_) return;
        mediaEjected_ = ejected;
        plotterview_setTapeEjected(ejected);
        if (targetDrive_) targetDrive_->setEjected(ejected);
    }
    std::function<void(std::uint16_t)> onColorChanged_;
    std::function<void()> onExitRequested_;
    std::function<void(bool, bool)> onTraceChanged_;
    std::function<void(std::uint8_t)> onFontSizeChanged_;
    std::function<void(std::uint8_t)> onBrightnessChanged_;
    std::function<void(std::uint8_t)> onColumnsChanged_;
    std::function<void(const std::string&, bool)> onDeviceToggled_;
    LoopbackTestFn onLoopbackTest_;
};

}  // namespace hipi