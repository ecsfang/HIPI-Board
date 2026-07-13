// UiDialog.hpp
#pragma once
#include "RA8875.hpp"
#include "Screen.hpp"
#include "ui_buttons.hpp"
#include "ff.h"
#include <vector>
#include <string>
#include <cstring>
#include <cctype>
#include <functional>


// Global trace flag, defined elsewhere (e.g. hipi.cpp) -- same convention
// as the existing global `SDOK`.
extern bool bTrace;

namespace hp82163 {

class UiDialog {
public:
    UiDialog(RA8875& display, Screen& screen) : d_(display), screen_(screen) {}

    // Called with the chosen filename once the user confirms it in the
    // "Open file?" dialog. Decouples UiDialog from whatever device class
    // (e.g. CDrive) actually acts on the file -- just wire it up in main:
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

    // Called with the newly chosen trace value whenever the user picks
    // True/False in the "Trace" menu (after the global bTrace has already
    // been updated). Useful for persisting the choice.
    void setTraceChangedCallback(std::function<void(bool)> cb) {
        onTraceChanged_ = std::move(cb);
    }

    bool isOpen() const { return state_ != State::Closed; }

    // Anropas fran huvudloopen nar en knapp-touch upptäcks.
    void handleButton(Button b) {
        cdc0_printf("State: %d Button: %d\r\n", state_, b);

        switch (state_) {
            case State::Closed:
                if (b == Button::Ok) openMainMenu();
                break;

            case State::MainMenu:
                if (b == Button::Up)   moveSelection(-1, kMainMenuCount);
                if (b == Button::Down) moveSelection(+1, kMainMenuCount);
                if (b == Button::Ok)   enterMainMenuItem();
                if (b == Button::X)    close();
                break;

            case State::ColorPicker:
                if (b == Button::Up)   moveSelection(-1, kColorCount);
                if (b == Button::Down) moveSelection(+1, kColorCount);
                if (b == Button::Ok)   { applyColor(selected_); openMainMenu(); }
                if (b == Button::X)    openMainMenu();
                break;

            case State::FilePicker:
                if (b == Button::Up)   moveSelection(-1, static_cast<int>(files_.size()));
                if (b == Button::Down) moveSelection(+1, static_cast<int>(files_.size()));
                if (b == Button::Ok)   openConfirmFile(files_[selected_]);
                if (b == Button::X)    openMainMenu();
                break;

            case State::ConfirmFile:
                if (b == Button::Ok) { applyFile(pendingFile_); openMainMenu(); }
                if (b == Button::X)  openFilePicker();
                break;

            case State::TraceMenu:
                if (b == Button::Up)   moveSelection(-1, kTraceCount);
                if (b == Button::Down) moveSelection(+1, kTraceCount);
                if (b == Button::Ok)   { applyTrace(selected_); openMainMenu(); }
                if (b == Button::X)    openMainMenu();
                break;
        }
    }

private:
    enum class State { Closed, MainMenu, ColorPicker, FilePicker, ConfirmFile, TraceMenu };

    static constexpr const char* kMainMenuLabels[] = { "Textcolor", "Select file", "Trace" };
    static constexpr int kMainMenuCount = 3;

    static constexpr std::uint16_t kColors[]     = { 0xFFFF, 0xFFE0, 0x07E0, 0x07FF, 0xF800 };
    static constexpr const char*   kColorLabels[] = { "White", "Yellow", "Green", "Cyan", "Red" };
    static constexpr int kColorCount = 5;

    static constexpr const char* kTraceLabels[] = { "False", "True" };
    static constexpr int kTraceCount = 2;

    static constexpr int kBoxX = 60, kBoxY = 80, kBoxW = 400, kBoxH = 260;
    static constexpr int kBorderThickness = 3;
    static constexpr int kCornerRadius    = 14;

    // Samma gula/oranga som anvands i buttons.bmp. OBS: RA8875 i 8bpp-lage
    // verkar tolka det grona faltets tre understa bitar (mod 8), inte de tre
    // oversta som man kunde tro fran en vanlig RGB565->RGB332-konvertering.
    // Darfor valjer vi ett G-varde som redan ligger under 8 (ingen wrap-around
    // mojlig) istallet for att harleda det fran den riktiga BMP-fargen rakt av.
    static constexpr std::uint16_t kMenuYellow = 0xF9B0;

    void openMainMenu() {
        screen_.suspend();  // idempotent if already open; stops screen_.pr_char()
                             // from drawing over the dialog while it's showing
        state_ = State::MainMenu;
        selected_ = 0;
        drawBox();
        for (int i = 0; i < kMainMenuCount; ++i) drawRow(i, kMainMenuLabels[i]);
    }

    void enterMainMenuItem() {
        if (selected_ == 0) {
            state_ = State::ColorPicker;
            selected_ = 0;
            drawBox();
            for (int i = 0; i < kColorCount; ++i) drawRow(i, kColorLabels[i]);
        } else if (selected_ == 1) {
            openFilePicker();
        } else {
            openTraceMenu();
        }
    }

    void openTraceMenu() {
        state_ = State::TraceMenu;
        selected_ = bTrace ? 1 : 0;  // pre-select whatever bTrace currently is
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
        for (std::size_t i = 0; i < files_.size() && i < 10; ++i) {
            drawRow(static_cast<int>(i), files_[i].c_str());
        }
    }

    void openConfirmFile(const std::string& filename) {
        pendingFile_ = filename;
        state_ = State::ConfirmFile;
        drawBox();
        drawConfirmText();
    }

    void drawConfirmText() {
        d_.txtColor(0xFFFF, 0x0000);
        d_.txtSetCursor(kBoxX + 20, kBoxY + 20);
        d_.txtWrite("Open file?");
        d_.txtSetCursor(kBoxX + 20, kBoxY + 48);
        d_.txtWrite(pendingFile_.c_str());
        d_.txtSetCursor(kBoxX + 20, kBoxY + 90);
        d_.txtWrite("OK = yes    X = cancel");
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

    void applyTrace(int index) {
        bTrace = (index == 1);  // index 0 = "False", index 1 = "True"
        cdc0_printf("\r\n * Trace: %s", bTrace ? "true" : "false");
        if (onTraceChanged_) onTraceChanged_(bTrace);
    }

    void applyFile(const std::string& filename) {
        cdc0_printf("\r\n * Selected file: %s", filename.c_str());
        if (onFileSelected_) onFileSelected_(filename);
    }

    void drawBox() {
        // Tjock, rundad ram: fyll hela rutan gul (rundade horn), lagg sedan
        // en mindre svart rundad rektangel ovanpa, forskjuten med
        // kBorderThickness pa varje sida -> kvar blir en jamn gul kant.
        d_.fillRoundRect(kBoxX, kBoxY, kBoxW, kBoxH, kCornerRadius, kMenuYellow);
        const int innerRadius = kCornerRadius > kBorderThickness
                                     ? kCornerRadius - kBorderThickness : 0;
        d_.fillRoundRect(kBoxX + kBorderThickness, kBoxY + kBorderThickness,
                          kBoxW - 2 * kBorderThickness, kBoxH - 2 * kBorderThickness,
                          innerRadius, 0x0000);
    }

    void drawRow(int index, const char* label) {
        d_.txtSetCursor(kBoxX + 20, kBoxY + 20 + index * 24);
        d_.txtColor(index == selected_ ? 0x0000 : 0xFFFF,
                    index == selected_ ? kMenuYellow : 0x0000);
        d_.txtWrite(label);
    }

    void highlightRow(int index, bool /*selected*/) {
        // drawRow tittar redan pa selected_ for att valja fargschema,
        // sa vi behover bara peka den pa ratt label-kalla for aktivt state.
        switch (state_) {
            case State::MainMenu:
                if (index >= 0 && index < kMainMenuCount)
                    drawRow(index, kMainMenuLabels[index]);
                break;
            case State::ColorPicker:
                if (index >= 0 && index < kColorCount)
                    drawRow(index, kColorLabels[index]);
                break;
            case State::FilePicker:
                if (index >= 0 && index < static_cast<int>(files_.size()))
                    drawRow(index, files_[index].c_str());
                break;
            case State::TraceMenu:
                if (index >= 0 && index < kTraceCount)
                    drawRow(index, kTraceLabels[index]);
                break;
            case State::Closed:
                break;
            case State::ConfirmFile:
                break;
        }
    }

    RA8875& d_;
    Screen& screen_;
    State state_ = State::Closed;
    int selected_ = 0;
    std::vector<std::string> files_;
    std::string pendingFile_;
    std::function<void(const std::string&)> onFileSelected_;
    std::function<void(std::uint16_t)> onColorChanged_;
    std::function<void(bool)> onTraceChanged_;
};

}  // namespace hp82163