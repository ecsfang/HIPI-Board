// UiDialog.hpp
#pragma once
#include "RA8875.hpp"
#include "Screen.hpp"
#include "ui_buttons.hpp"
#include "ff.h"
#include <vector>
#include <string>

namespace hp82163 {

class UiDialog {
public:
    UiDialog(RA8875& display, Screen& screen) : d_(display), screen_(screen) {}

    bool isOpen() const { return state_ != State::Closed; }

    // Anropas fran huvudloopen nar en knapp-touch upptäcks.
    void handleButton(Button b) {
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
                if (b == Button::Ok)   { applyFile(files_[selected_]); openMainMenu(); }
                if (b == Button::X)    openMainMenu();
                break;
        }
    }

private:
    enum class State { Closed, MainMenu, ColorPicker, FilePicker };

    static constexpr const char* kMainMenuLabels[] = { "Textfarg", "Valj fil" };
    static constexpr int kMainMenuCount = 2;

    static constexpr std::uint16_t kColors[]     = { 0xFFFF, 0xFFE0, 0x07E0, 0x07FF, 0xF800 };
    static constexpr const char*   kColorLabels[] = { "Vit", "Gul", "Gron", "Cyan", "Rod" };
    static constexpr int kColorCount = 5;

    static constexpr int kBoxX = 60, kBoxY = 80, kBoxW = 400, kBoxH = 260;

    void openMainMenu() {
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
        } else {
            openFilePicker();
        }
    }

    void openFilePicker() {
        files_.clear();
        DIR dir;
        if (f_opendir(&dir, "0:/") == FR_OK) {
            FILINFO info;
            while (f_readdir(&dir, &info) == FR_OK && info.fname[0] != 0) {
                if (!(info.fattrib & AM_DIR)) files_.push_back(info.fname);
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

    void close() {
        state_ = State::Closed;
        screen_.full();   // ritar om textytan fran lines_-bufferten
    }

    void moveSelection(int delta, int count) {
        if (count == 0) return;
        int old = selected_;
        selected_ = (selected_ + delta + count) % count;
        highlightRow(old, false);
        highlightRow(selected_, true);
    }

    void applyColor(int index) {
        d_.txtColor(kColors[index], 0);   // satter FG-fargregistret direkt i hardvaran
    }

    void applyFile(const std::string& filename) {
        // Hook: gor har det du faktiskt vill nar en fil valts,
        // t.ex. rita om buttons.bmp fran en annan fil, eller ladda config.
        cdc0_printf("\r\n * Vald fil: %s", filename.c_str());
    }

    void drawBox() {
        d_.fillRect(kBoxX, kBoxY, kBoxW, kBoxH, 0x0000);   // svart bakgrund
        d_.rect(kBoxX, kBoxY, kBoxW, kBoxH, 0xFFE0);       // gul kant
    }

    void drawRow(int index, const char* label) {
        d_.txtSetCursor(kBoxX + 20, kBoxY + 20 + index * 24);
        d_.txtColor(index == selected_ ? 0x0000 : 0xFFFF,
                    index == selected_ ? 0xFFE0 : 0x0000);
        d_.txtWrite(label);
    }

    void highlightRow(int index, bool selected) {
        // Enklast: rita om hela raden med rätt fargschema
        // (byt ut label-listan mot ratt kalla beroende pa state_ om du vill undvika dubbelarbete)
    }

    RA8875& d_;
    Screen& screen_;
    State state_ = State::Closed;
    int selected_ = 0;
    std::vector<std::string> files_;
};

}  // namespace hp82163