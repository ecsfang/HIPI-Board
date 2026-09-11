// UiDialog.hpp
#pragma once
#include "display_config.h"
#include "Screen.hpp"
#include "ui_buttons.hpp"
#include "usb_serial.h"  // LOGF, used by applyTrace()/applyFile()
#include "hpil.h"        // CDevice, for the "Devices" enable/disable menu
#include "plotterview.h" // DisplayOutput, for the "Display" output-mode menu
#include "ff.h"
#include "pico/bootrom.h" // reset_usb_boot(), for the "Bootsel mode" menu item
#include "usb_msc.h"      // enterUsbMscMode()/exitUsbMscMode(), for "Connect to PC"
#include <vector>
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

    // Samma gula/oranga som anvands i buttons.bmp (#E8B800 -> RGB565).
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

    // Thick, rounded frame: fill the whole box yellow (rounded corners),
    // then lay a smaller rounded black rectangle on top, inset by
    // BorderThickness on each side -> leaves an even yellow border.
    inline void draw(DisplayDriver* d, int x = X, int y = Y, int w = W, int h = H) {
        d->txtSize(TextScale);   // force menu's own scale, independent of Screen's
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

    // Called with the chosen filename once the user confirms it in the
    // "Open file?" dialog. Decouples UiDialog from whatever device class
    // (e.g. CTape) actually acts on the file -- just wire it up in main:
    //
    //   dialog.setFileSelectedCallback([&drive](const std::string& name) {
    //       drive.select(name);
    //   });
    void setFileSelectedCallback(std::function<void(const std::string&)> cb) {
        onFileSelected_ = std::move(cb);
    }

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
    void setCurrentFile(const std::string& filename) {
        lastAppliedFile_ = filename;
    }

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

    bool isOpen() const { return state_ != State::Closed; }

    // Anropas fran huvudloopen nar en knapp-touch upptäcks.
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

    void handleButton(Button b) {
        if (b == Button::Shift) {
            shiftPending_ = true;   // latch for the *next* button press
            return;
        }
        const bool shifted = shiftPending_;
        shiftPending_ = false;      // consumed by this button press, whatever it is

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
            case State::Closed:
                if (b == Button::Ok) {
                    openMainMenu();
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

            case State::MainMenu:
                if (b == Button::Up)   moveSelection(-1, kMainMenuCount);
                if (b == Button::Down) moveSelection(+1, kMainMenuCount);
                if (b == Button::Ok)   enterMainMenuItem();
                if (b == Button::X)    close();
                break;

            case State::ConfigMenu:
                if (b == Button::Up)   moveSelection(-1, kConfigMenuCount);
                if (b == Button::Down) moveSelection(+1, kConfigMenuCount);
                if (b == Button::Ok)   enterConfigMenuItem();
                if (b == Button::X)    openMainMenu();
                break;

            case State::SettingsMenu:
                if (b == Button::Up)   moveSelection(-1, kSettingsMenuCount);
                if (b == Button::Down) moveSelection(+1, kSettingsMenuCount);
                if (b == Button::Ok)   enterSettingsMenuItem();
                if (b == Button::X)    openMainMenu();
                break;

            case State::ColorPicker:
                if (b == Button::Up)   moveSelection(-1, kColorCount);
                if (b == Button::Down) moveSelection(+1, kColorCount);
                if (b == Button::Ok)   { applyColor(selected_); openSettingsMenu(); }
                if (b == Button::X)    openSettingsMenu();
                break;

            case State::FontSizeMenu:
                if (b == Button::Up)   moveSelection(-1, kFontSizeCount);
                if (b == Button::Down) moveSelection(+1, kFontSizeCount);
                if (b == Button::Ok)   { applyFontSize(selected_); openSettingsMenu(); }
                if (b == Button::X)    openSettingsMenu();
                break;

            case State::BrightnessMenu:
                if (b == Button::Up)   moveSelection(-1, kBrightnessCount);
                if (b == Button::Down) moveSelection(+1, kBrightnessCount);
                if (b == Button::Ok)   { applyBrightness(selected_); openSettingsMenu(); }
                if (b == Button::X)    openSettingsMenu();
                break;

            case State::ColumnsMenu:
                if (b == Button::Up)   moveSelection(-1, kColumnsCount);
                if (b == Button::Down) moveSelection(+1, kColumnsCount);
                if (b == Button::Ok)   { applyColumns(selected_); openSettingsMenu(); }
                if (b == Button::X)    openSettingsMenu();
                break;

            case State::FilePicker:
                if (b == Button::Up)   moveFileSelection(-1);
                if (b == Button::Down) moveFileSelection(+1);
                if (b == Button::Ok)   openConfirmFile(files_[selected_]);
                if (b == Button::X)    openConfigMenu();
                break;

            case State::ConfirmFile:
                if (b == Button::Ok) { applyFile(pendingFile_); openConfigMenu(); }
                if (b == Button::X)  openFilePicker();
                break;

            case State::TraceMenu:
                if (b == Button::Up)   moveSelection(-1, kTraceCount);
                if (b == Button::Down) moveSelection(+1, kTraceCount);
                if (b == Button::Ok)   { applyTrace(selected_); openConfigMenu(); }
                if (b == Button::X)    openConfigMenu();
                break;

            case State::DeviceList:
                if (b == Button::Up)   moveSelection(-1, static_cast<int>(devices.size()));
                if (b == Button::Down) moveSelection(+1, static_cast<int>(devices.size()));
                if (b == Button::Ok)   toggleDevice(selected_);
                if (b == Button::X)    openMainMenu();
                break;

            case State::DisplayMenu:
                if (b == Button::Up)   moveSelection(-1, kDisplayMenuCount);
                if (b == Button::Down) moveSelection(+1, kDisplayMenuCount);
                if (b == Button::Ok)   enterDisplayMenuItem();
                if (b == Button::X)    openMainMenu();
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
                d_->showPipOverlay(MenuFrame::X, MenuFrame::Y, MenuFrame::W, MenuFrame::H);
            } else {
                // openMainMenu() (and every other open*Menu()) hides the
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
    enum class State {
        Closed, MainMenu, ConfigMenu, SettingsMenu,
        ColorPicker, FontSizeMenu, BrightnessMenu, ColumnsMenu,
        FilePicker, ConfirmFile, TraceMenu, DeviceList, DisplayMenu
    };

    static constexpr const char* kMainMenuLabels[] = { "Config", "Settings", "Devices", "Display" };
    static constexpr int kMainMenuCount = 4;

    static constexpr const char* kConfigMenuLabels[] = { "Select file", "Trace", "Connect to PC" };
    static constexpr int kConfigMenuCount = 3;

    static constexpr const char* kSettingsMenuLabels[] = { "Textcolor", "Font size", "Brightness", "Columns", "Bootsel mode" };
    static constexpr int kSettingsMenuCount = 5;

    static constexpr std::uint16_t kColors[]     = { 0xFFFF, 0xFFE0, 0x07E0, 0x07FF, 0xF800 };
    static constexpr const char*   kColorLabels[] = { "White", "Yellow", "Green", "Cyan", "Red" };
    static constexpr int kColorCount = 5;

    // Matches Screen's size_ (0..3, built-in CGRAM modes only -- the
    // custom "fon" mode 4 isn't offered here).
    static constexpr const char* kFontSizeLabels[] = { "0", "1", "2", "3" };
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
    static constexpr const char* kDisplayMenuLabels[] = { "Display", "Plotter", "Clear plotter", "Clear screen" };
    static constexpr int kDisplayMenuCount = 4;

    void openMainMenu() {
#ifndef DISPLAY_7INCH
        // Only needed on the 5" (RA8875) path -- the 7" (LT7683) path
        // draws the menu to its own separate PIP layer instead (see
        // handleButton()'s own overlay-wrapping), so the live text
        // screen never needs pausing at all; it just gets visually
        // covered wherever the PIP overlay sits on top.
        screen_.suspend();  // idempotent if already open; stops screen_.pr_char()
                             // from drawing over the dialog while it's showing
#endif
        // Hide the hardware blinking cursor too -- suspend() alone only
        // stops FURTHER drawing, it doesn't touch the cursor already left
        // blinking at wherever Screen's text last positioned it, which
        // otherwise shows through/behind the menu box's own text.
        // close() doesn't need a matching "show it again" call: resume()
        // -> full() already ends with set_cur(), which re-asserts
        // visibility from Screen's own cv_ state (see its own comment).
        d_->setTextCursorVisible(false, false);
        state_ = State::MainMenu;
        selected_ = 0;
        drawBox();
        for (int i = 0; i < kMainMenuCount; ++i) drawRow(i, kMainMenuLabels[i]);
    }

    void enterMainMenuItem() {
        if (selected_ == 0)      openConfigMenu();
        else if (selected_ == 1) openSettingsMenu();
        else if (selected_ == 2) openDeviceList();
        else                     openDisplayMenu();
    }

    void openDisplayMenu() {
        state_ = State::DisplayMenu;
        selected_ = (plotterview_output() == DisplayOutput::Plotter) ? 1 : 0;
        drawBox();
        for (int i = 0; i < kDisplayMenuCount; ++i) drawRow(i, kDisplayMenuLabels[i]);
    }

    void enterDisplayMenuItem() {
        if (selected_ == 0) {
            plotterview_setOutput(DisplayOutput::Display);
            close();
        } else if (selected_ == 1) {
            plotterview_setOutput(DisplayOutput::Plotter);
            close();
        } else if (selected_ == 2) {
            plotterview_clearPlotter();
            close();
        } else {
            // Clear screen -- immediate action, like "Clear plotter"
            // above: wipes the text buffer, clears the panel, and homes
            // the cursor (Screen::clear()'s own documented behaviour,
            // matching the HP82163's own "Clear Device" semantics).
            // Wrapped in withMainCanvas() -- see its own comment -- since
            // this runs from within the menu's own button handling,
            // where (on the 7" panel) the canvas is still pointed at the
            // menu's own overlay layer; without it, clear()'s own redraw
            // of the now-empty buffer would silently happen on the
            // invisible menu layer instead of the actual panel.
            withMainCanvas([&]{ screen_.clear(); });
            close();
        }
    }

    void openConfigMenu() {
        state_ = State::ConfigMenu;
        selected_ = 0;
        drawBox();
        drawConfigMenuRows();
    }

    // Row 2 ("Connect to PC" / "Disconnect from PC") reflects the
    // current mode dynamically -- everything else just uses the fixed
    // labels. Shared between openConfigMenu() (full redraw) and
    // highlightRow()'s own State::ConfigMenu case (single-row redraw
    // during Up/Down navigation).
    void drawConfigMenuRows() {
        drawRow(0, kConfigMenuLabels[0]);
        drawRow(1, kConfigMenuLabels[1]);
        drawRow(2, usbMscModeActive() ? "Disconnect from PC" : "Connect to PC");
    }

    void enterConfigMenuItem() {
        if (selected_ == 0) {
            openFilePicker();
        } else if (selected_ == 1) {
            openTraceMenu();
        } else {
            // "Connect to PC" / "Disconnect from PC" -- toggles USB MSC
            // mode (see usb_msc.h for the full story). Shows explicit
            // feedback either way, then returns to this same menu (with
            // its label now reflecting the new state) rather than
            // acting like "Bootsel mode"'s one-way, device-reboots
            // action -- this one's reversible, and the user needs to
            // see the result to know it's safe to unplug/plug back in.
            const bool wasActive = usbMscModeActive();
            const bool nowActive = wasActive ? (exitUsbMscMode(), false) : enterUsbMscMode();
            drawBox();
            d_->txtColor(0xFFFF, 0x0000);
            d_->txtSetCursor(MenuFrame::X + 20, MenuFrame::Y + 20);
            if (wasActive) {
                d_->txtWrite("Disconnected from PC.");
                d_->txtSetCursor(MenuFrame::X + 20, MenuFrame::Y + 20 + MenuFrame::RowPitch);
                d_->txtWrite("HP-IL drive access resumed.");
            } else if (nowActive) {
                d_->txtWrite("Connected -- SD card is now");
                d_->txtSetCursor(MenuFrame::X + 20, MenuFrame::Y + 20 + MenuFrame::RowPitch);
                d_->txtWrite("visible as a USB drive on the PC.");
                d_->txtSetCursor(MenuFrame::X + 20, MenuFrame::Y + 20 + 2 * MenuFrame::RowPitch);
                d_->txtWrite("HP-IL drive access paused.");
            } else {
                d_->txtWrite("Couldn't connect -- no SD card?");
            }
            sleep_ms(1500);
            openConfigMenu();
        }
    }

    void openSettingsMenu() {
        state_ = State::SettingsMenu;
        selected_ = 0;
        drawBox();
        for (int i = 0; i < kSettingsMenuCount; ++i) drawRow(i, kSettingsMenuLabels[i]);
    }

    void enterSettingsMenuItem() {
        if (selected_ == 0) {
            state_ = State::ColorPicker;
            selected_ = 0;  // "White" if no exact match found below
            for (int i = 0; i < kColorCount; ++i) {
                if (kColors[i] == screen_.color()) { selected_ = i; break; }
            }
            drawBox();
            drawColorList();
        } else if (selected_ == 1) {
            openFontSizeMenu();
        } else if (selected_ == 2) {
            openBrightnessMenu();
        } else if (selected_ == 3) {
            openColumnsMenu();
        } else {
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
            d_->showPipOverlay(MenuFrame::X, MenuFrame::Y, MenuFrame::W, MenuFrame::H);
#endif
            sleep_ms(1500);  // long enough to actually read before reboot
            reset_usb_boot(0, 0);
        }
    }

    void openFontSizeMenu() {
        state_ = State::FontSizeMenu;
        selected_ = screen_.size() < kFontSizeCount ? screen_.size() : 0;
        drawBox();
        for (int i = 0; i < kFontSizeCount; ++i) drawRow(i, kFontSizeLabels[i]);
    }

    void openBrightnessMenu() {
        state_ = State::BrightnessMenu;
        selected_ = closestBrightnessIndex(screen_.brightness());
        drawBox();
        for (int i = 0; i < kBrightnessCount; ++i) drawRow(i, kBrightnessLabels[i]);
    }

    void openColumnsMenu() {
        state_ = State::ColumnsMenu;
        selected_ = 0;  // "Auto" if no exact match found below
        for (int i = 0; i < kColumnsCount; ++i) {
            if (kColumnsValues[i] == screen_.columnsOverride()) { selected_ = i; break; }
        }
        drawBox();
        for (int i = 0; i < kColumnsCount; ++i) drawRow(i, kColumnsLabels[i]);
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

    void openTraceMenu() {
        state_ = State::TraceMenu;
        // Off (0): trace=false, debug=false. On (1): trace=true, debug=false.
        // Extended (2): trace=true, debug=true.
        selected_ = bExtTrace ? 2 : (bTrace ? 1 : 0);
        drawBox();
        for (int i = 0; i < kTraceCount; ++i) drawRow(i, kTraceLabels[i]);
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
    static constexpr int kMaxFilesShown =
        (MenuFrame::H - 40) / MenuFrame::RowPitch;

    void openFilePicker() {
        files_.clear();
        DIR dir;
        if (f_opendir(&dir, "") == FR_OK) {
            FILINFO info;
            while (f_readdir(&dir, &info) == FR_OK && info.fname[0] != 0) {
                if (!(info.fattrib & AM_DIR) && hasExtension(info.fname, ".dat")) {
                    files_.push_back(info.fname);
                }
            }
            f_closedir(&dir);
        }
        state_ = State::FilePicker;
        // Find and highlight the last file actually applied, same idea
        // as every other multi-choice menu (Textcolor/Font size/
        // Brightness/Columns) -- falls back to index 0 (the first file
        // in the list) if it's not found (e.g. nothing's ever been
        // applied yet this session, or that file's been deleted/renamed
        // since).
        selected_ = 0;
        for (std::size_t i = 0; i < files_.size(); ++i) {
            if (files_[i] == lastAppliedFile_) { selected_ = static_cast<int>(i); break; }
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
        drawBox();
        drawFileList();
    }

    // Redraws the currently-scrolled-to window of files_ into the menu
    // box -- shared by openFilePicker() (initial draw) and
    // moveFileSelection() (whenever scrolling is needed after Up/Down).
    void drawFileList() {
        // See drawColorRow()'s own comment for why this is needed here
        // too -- called on its own during Up/Down navigation (see
        // moveFileSelection()), without drawBox() running again first.
        // Once per call (not per row) is enough -- every row below uses
        // the same scale.
        d_->txtSize(MenuFrame::TextScale);
        for (int row = 0; row < kMaxFilesShown; ++row) {
            const std::size_t fileIndex = static_cast<std::size_t>(fileScrollOffset_ + row);
            if (fileIndex >= files_.size()) break;
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
            const bool isSelected = (static_cast<int>(fileIndex) == selected_);
            d_->txtColor(isSelected ? 0x0000 : 0xFFFF, isSelected ? MenuFrame::Yellow : 0x0000);
            d_->txtWrite(files_[fileIndex].c_str());
        }
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
        drawBox();
        for (std::size_t i = 0; i < deviceLabels_.size() && i < static_cast<std::size_t>(kMaxFilesShown); ++i) {
            drawRow(static_cast<int>(i), deviceLabels_[i].c_str());
        }
    }

    static std::string deviceLabel(CDevice* dev) {
        return std::string(dev->name()) + (dev->enabled() ? " [ON]" : " [OFF]");
    }

    void toggleDevice(int index) {
        if (index < 0 || index >= static_cast<int>(devices.size())) return;
        CDevice* dev = devices[index];
        dev->toggleEnabled();
        deviceLabels_[static_cast<std::size_t>(index)] = deviceLabel(dev);
        drawRow(index, deviceLabels_[static_cast<std::size_t>(index)].c_str());
        if (onDeviceToggled_) onDeviceToggled_(dev->name(), dev->enabled());
    }

    void openConfirmFile(const std::string& filename) {
        pendingFile_ = filename;
        state_ = State::ConfirmFile;
        drawBox();
        drawConfirmText();
    }

    void drawConfirmText() {
        d_->txtColor(0xFFFF, 0x0000);
        d_->txtSetCursor(MenuFrame::X + 20, MenuFrame::Y + 20);
        d_->txtWrite("Open file?");
        d_->txtSetCursor(MenuFrame::X + 20, MenuFrame::Y + 20 + MenuFrame::RowPitch);
        d_->txtWrite(pendingFile_.c_str());
        d_->txtSetCursor(MenuFrame::X + 20, MenuFrame::Y + 20 + 2 * MenuFrame::RowPitch);
        d_->txtWrite("OK = yes    X = cancel");
    }

    void close() {
        state_ = State::Closed;
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
        d_->hidePipOverlay();
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
        if (plotterview_output() == DisplayOutput::Plotter) {
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
        LOGF("\r\n * Selected file: %s", filename.c_str());
        lastAppliedFile_ = filename;  // see openFilePicker()'s own comment
        if (onFileSelected_) onFileSelected_(filename);
    }

    // Draws every entry in kColorLabels using ITS OWN colour (kColors[i])
    // as the text colour, rather than the generic white/black scheme
    // drawRow() uses everywhere else -- makes each option visually show
    // what picking it actually looks like, not just its name. Selection
    // is shown via a neutral dark grey background instead of drawRow()'s
    // own yellow -- a fixed highlight colour couldn't work here (e.g.
    // yellow text would vanish against a yellow highlight), while the
    // background/text colours staying independent works for all of them,
    // including White and Yellow themselves.
    // See drawColorList()'s own comment for why this differs from
    // drawRow(). Single-row version so highlightRow() (redraws just the
    // one row whose highlight changed, on Up/Down) doesn't need to
    // redraw all of kColorLabels every time the way drawColorList()
    // itself does for the initial, full draw.
    void drawColorRow(int i) {
        constexpr std::uint16_t kSelectedBg = 0x39E7;  // neutral dark grey
        // Explicitly (re-)asserts the menu's own fixed text scale --
        // see Screen::draw_letter()'s own comment for the full story on
        // why neither side can rely on the shared hardware register
        // still being whatever it last set it to. Needed here
        // specifically because this (like drawRow()/drawFileList()) can
        // be called on its own during Up/Down navigation within an
        // already-open menu, without drawBox()/MenuFrame::draw() (the
        // only other thing that sets this) running again first.
        d_->txtSize(MenuFrame::TextScale);
        d_->txtSetCursor(MenuFrame::X + 20, MenuFrame::Y + 20 + i * MenuFrame::RowPitch);
        d_->txtColor(kColors[i], i == selected_ ? kSelectedBg : 0x0000);
        d_->txtWrite(kColorLabels[i]);
    }

    void drawColorList() {
        for (int i = 0; i < kColorCount; ++i) drawColorRow(i);
    }

    void drawBox() {
        MenuFrame::draw(d_);
    }

    void drawRow(int index, const char* label) {
        // See drawColorRow()'s own comment for why this is needed here
        // too -- called on its own during Up/Down navigation, without
        // drawBox() running again first.
        d_->txtSize(MenuFrame::TextScale);
        d_->txtSetCursor(MenuFrame::X + 20, MenuFrame::Y + 20 + index * MenuFrame::RowPitch);
        d_->txtColor(index == selected_ ? 0x0000 : 0xFFFF,
                    index == selected_ ? MenuFrame::Yellow : 0x0000);
        d_->txtWrite(label);
    }

    void highlightRow(int index, bool /*selected*/) {
        // drawRow tittar redan pa selected_ for att valja fargschema,
        // sa vi behover bara peka den pa ratt label-kalla for aktivt state.
        switch (state_) {
            case State::MainMenu:
                if (index >= 0 && index < kMainMenuCount)
                    drawRow(index, kMainMenuLabels[index]);
                break;
            case State::ConfigMenu:
                if (index == 2) drawRow(2, usbMscModeActive() ? "Disconnect from PC" : "Connect to PC");
                else if (index >= 0 && index < kConfigMenuCount)
                    drawRow(index, kConfigMenuLabels[index]);
                break;
            case State::SettingsMenu:
                if (index >= 0 && index < kSettingsMenuCount)
                    drawRow(index, kSettingsMenuLabels[index]);
                break;
            case State::ColorPicker:
                if (index >= 0 && index < kColorCount)
                    drawColorRow(index);
                break;
            case State::FontSizeMenu:
                if (index >= 0 && index < kFontSizeCount)
                    drawRow(index, kFontSizeLabels[index]);
                break;
            case State::BrightnessMenu:
                if (index >= 0 && index < kBrightnessCount)
                    drawRow(index, kBrightnessLabels[index]);
                break;
            case State::ColumnsMenu:
                if (index >= 0 && index < kColumnsCount)
                    drawRow(index, kColumnsLabels[index]);
                break;
            case State::FilePicker:
                if (index >= 0 && index < static_cast<int>(files_.size()))
                    drawRow(index, files_[index].c_str());
                break;
            case State::TraceMenu:
                if (index >= 0 && index < kTraceCount)
                    drawRow(index, kTraceLabels[index]);
                break;
            case State::DeviceList:
                if (index >= 0 && index < static_cast<int>(deviceLabels_.size()))
                    drawRow(index, deviceLabels_[static_cast<std::size_t>(index)].c_str());
                break;
            case State::DisplayMenu:
                if (index >= 0 && index < kDisplayMenuCount)
                    drawRow(index, kDisplayMenuLabels[index]);
                break;
            case State::Closed:
                break;
            case State::ConfirmFile:
                break;
        }
    }

    DisplayDriver* d_;
    Screen& screen_;
    State state_ = State::Closed;
    int selected_ = 0;
    bool shiftPending_ = false;   // latched by Button::Shift (any state),
                                    // consumed by the next button press
    std::vector<std::string> files_;
    // See openFilePicker()'s own comment for why these exist -- neither
    // did before.
    int fileScrollOffset_ = 0;
    std::string lastAppliedFile_;
    std::string pendingFile_;
    std::vector<std::string> deviceLabels_;
    std::function<void(const std::string&)> onFileSelected_;
    std::function<void(std::uint16_t)> onColorChanged_;
    std::function<void()> onExitRequested_;
    std::function<void(bool, bool)> onTraceChanged_;
    std::function<void(std::uint8_t)> onFontSizeChanged_;
    std::function<void(std::uint8_t)> onBrightnessChanged_;
    std::function<void(std::uint8_t)> onColumnsChanged_;
    std::function<void(const std::string&, bool)> onDeviceToggled_;
};

}  // namespace hipi