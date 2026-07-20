// UiDialog.hpp
#pragma once
#include "RA8875.hpp"
#include "Screen.hpp"
#include "ui_buttons.hpp"
#include "usb_serial.h"  // LOGF, used by applyTrace()/applyFile()
#include "hpil.h"        // CDevice, for the "Devices" enable/disable menu
#include "ff.h"
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

namespace hp82163 {

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
    inline void draw(RA8875* d, int x = X, int y = Y, int w = W, int h = H) {
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
    UiDialog(RA8875* display, Screen& screen) : d_(display), screen_(screen) {}

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
    void handleButton(Button b) {

        if (b == Button::Shift) {
            shiftPending_ = true;   // latch for the *next* button press
            return;
        }
        const bool shifted = shiftPending_;
        shiftPending_ = false;      // consumed by this button press, whatever it is

        // Shift+OK ("EXIT" on the button graphic) leaves the menu entirely,
        // from any depth -- unlike X ("<--"), which only goes up one level.
        if (shifted && b == Button::Ok && isOpen()) {
            close();
            return;
        }

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
                if (b == Button::Up)   moveSelection(-1, static_cast<int>(files_.size()));
                if (b == Button::Down) moveSelection(+1, static_cast<int>(files_.size()));
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
        }
    }

private:
    enum class State {
        Closed, MainMenu, ConfigMenu, SettingsMenu,
        ColorPicker, FontSizeMenu, BrightnessMenu, ColumnsMenu,
        FilePicker, ConfirmFile, TraceMenu, DeviceList
    };

    static constexpr const char* kMainMenuLabels[] = { "Config", "Settings", "Devices" };
    static constexpr int kMainMenuCount = 3;

    static constexpr const char* kConfigMenuLabels[] = { "Select file", "Trace" };
    static constexpr int kConfigMenuCount = 2;

    static constexpr const char* kSettingsMenuLabels[] = { "Textcolor", "Font size", "Brightness", "Columns" };
    static constexpr int kSettingsMenuCount = 4;

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

    void openMainMenu() {
        screen_.suspend();  // idempotent if already open; stops screen_.pr_char()
                             // from drawing over the dialog while it's showing
        state_ = State::MainMenu;
        selected_ = 0;
        drawBox();
        for (int i = 0; i < kMainMenuCount; ++i) drawRow(i, kMainMenuLabels[i]);
    }

    void enterMainMenuItem() {
        if (selected_ == 0)      openConfigMenu();
        else if (selected_ == 1) openSettingsMenu();
        else                     openDeviceList();
    }

    void openConfigMenu() {
        state_ = State::ConfigMenu;
        selected_ = 0;
        drawBox();
        for (int i = 0; i < kConfigMenuCount; ++i) drawRow(i, kConfigMenuLabels[i]);
    }

    void enterConfigMenuItem() {
        if (selected_ == 0) openFilePicker();
        else                openTraceMenu();
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
            selected_ = 0;
            drawBox();
            for (int i = 0; i < kColorCount; ++i) drawRow(i, kColorLabels[i]);
        } else if (selected_ == 1) {
            openFontSizeMenu();
        } else if (selected_ == 2) {
            openBrightnessMenu();
        } else {
            openColumnsMenu();
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
        selected_ = 0;
        drawBox();
        for (std::size_t i = 0; i < files_.size() && i < static_cast<std::size_t>(kMaxFilesShown); ++i) {
            drawRow(static_cast<int>(i), files_[i].c_str());
        }
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
        screen_.resume();   // re-enables output and redraws the buffer,
                             // catching up on anything written while the
                             // dialog was open
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
        screen_.setTextSize(static_cast<std::uint8_t>(index));
        if (onFontSizeChanged_) onFontSizeChanged_(static_cast<std::uint8_t>(index));
    }

    void applyBrightness(int index) {
        const std::uint8_t level = kBrightnessLevels[index];
        screen_.setBrightness(level);
        if (onBrightnessChanged_) onBrightnessChanged_(level);
    }

    void applyColumns(int index) {
        const std::uint8_t cols = kColumnsValues[index];
        screen_.setColumns(cols);
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
        if (onFileSelected_) onFileSelected_(filename);
    }

    void drawBox() {
        MenuFrame::draw(d_);
    }

    void drawRow(int index, const char* label) {
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
                if (index >= 0 && index < kConfigMenuCount)
                    drawRow(index, kConfigMenuLabels[index]);
                break;
            case State::SettingsMenu:
                if (index >= 0 && index < kSettingsMenuCount)
                    drawRow(index, kSettingsMenuLabels[index]);
                break;
            case State::ColorPicker:
                if (index >= 0 && index < kColorCount)
                    drawRow(index, kColorLabels[index]);
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
            case State::Closed:
                break;
            case State::ConfirmFile:
                break;
        }
    }

    RA8875* d_;
    Screen& screen_;
    State state_ = State::Closed;
    int selected_ = 0;
    bool shiftPending_ = false;   // latched by Button::Shift (any state),
                                    // consumed by the next button press
    std::vector<std::string> files_;
    std::string pendingFile_;
    std::vector<std::string> deviceLabels_;
    std::function<void(const std::string&)> onFileSelected_;
    std::function<void(std::uint16_t)> onColorChanged_;
    std::function<void(bool, bool)> onTraceChanged_;
    std::function<void(std::uint8_t)> onFontSizeChanged_;
    std::function<void(std::uint8_t)> onBrightnessChanged_;
    std::function<void(std::uint8_t)> onColumnsChanged_;
    std::function<void(const std::string&, bool)> onDeviceToggled_;
};

}  // namespace hp82163
