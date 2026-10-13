// media_create_ui.h -- File picker -> Create media...: the dialogs that
// make a new, empty cassette/disk image (lif_create.h) on the SD card.
//
//   CREATE MEDIA   the name (pre-filled: the next free CASS01/DISK01...),
//                  the medium type and the directory size, what that
//                  gives, and Create / Cancel. While the file is written:
//                  a progress bar (Cancel deletes it again).
//   MEDIA NAME     an on-screen QWERTY keyboard for the name: A-Z, 0-9,
//                  '-' and '_', at most 24 characters (".DAT" is added).
//
// Touch works on everything; the button strip too: Up/Down move between
// Name, Type, Directory and Create, OK picks (the type and the directory
// step to the next value), X goes back. In MEDIA NAME: OK = done,
// X = delete the last character (back when there's none).
//
// Drawn with the menus' frame and title (menu_frame.h) over the screen
// left of the button strip. UiDialog (uidialog.hpp) owns it: it opens it,
// passes on touches, buttons and polls, shows the box (PIP window on the
// 7" panel, frameRect()) and, when a file was created, gives it to the drive.
#pragma once
#include "display_config.h"
#include "lif_create.h"
#include "ui_buttons.hpp"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace hipi {

enum class MediaAction { None, Cancel, Created };

class MediaCreateUi {
public:
    explicit MediaCreateUi(DisplayDriver* d);

    // The date for the new volume: true and filled in if the real-time
    // clock has it, else the date stays empty
    void setDateSource(std::function<bool(LifDate&)> f) { dateSource_ = std::move(f); }

    // Starts afresh on CREATE MEDIA (reads the free space on the SD card,
    // suggests a name). Draw with draw().
    void open();
    void draw();                         // the whole page (frame included)
    // Stops writing a file (it's deleted again) -- the menu was closed
    void cancel() { creator_.cancel(); }
    void frameRect(int& x, int& y, int& w, int& h) const;

    MediaAction tap(int x, int y);
    MediaAction button(Button b);
    // Main loop: writes the file a piece at a time, ends a key's flash
    MediaAction poll(std::uint32_t nowMs);
    // poll() has work to do (and may draw)
    bool busy() const { return creating() || flashKey_ >= 0; }

    bool creating() const { return creator_.state() == LifCreator::State::Running; }
    const std::string& createdFile() const { return created_; }    // "NAME.DAT"
    bool onKeyboard() const { return page_ == Page::Keyboard; }
    const std::string& name() const { return name_; }
    const std::string& message() const { return message_; }

    // Where things are (the tests tap them)
    enum class Ctl { Name, TypeLeft, TypeRight, DirLeft, DirRight, Cancel, Create };
    void center(Ctl c, int& x, int& y) const;
    static constexpr char kCancel = 1, kDel = 8, kOk = 13;     // the special keys
    bool keyCenter(char code, int& x, int& y) const;

private:
    enum class Page { Form, Keyboard };
    struct Rect {
        int x = 0, y = 0, w = 0, h = 0;
        bool hit(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
    };
    struct Key { Rect r; char code; int units; };

    const LifMediaType& type() const { return kLifMediaTypes[typeIdx_]; }
    int dirEntries() const;
    std::string problem() const;         // why Create can't be used now, "" if it can

    void setType(int idx);
    void stepDir(int delta);
    MediaAction create();
    MediaAction formOk();
    void openKeyboard();
    MediaAction keyPressed(char code);

    // Drawing
    void drawForm();
    void drawName();
    void drawChoice(int row, const std::string& value);
    void drawInfo();
    void drawMessage();
    void drawButtons();
    void drawProgress();
    void drawKeyboard();
    void drawField();
    void drawKey(const Key& k, bool pressed);
    void box(const Rect& r, std::uint16_t outline, std::uint16_t fill, int radius = 8);
    void text(int x, int y, const std::string& s, int scale, std::uint16_t fg, std::uint16_t bg);
    void textCentered(const Rect& r, const std::string& s, int scale, std::uint16_t fg, std::uint16_t bg);
    void arrow(const Rect& r, bool left, std::uint16_t col);

    DisplayDriver* d_;
    std::function<bool(LifDate&)> dateSource_;
    Page page_ = Page::Form;
    int typeIdx_ = 0, dirIdx_ = 0;
    std::string name_;                   // without .DAT
    bool nameSuggested_ = true;          // still the suggestion: follows the type
    std::string edit_;                   // MEDIA NAME: being typed
    int focus_ = 0;                      // Form: 0 name, 1 type, 2 directory, 3 Create
    std::string message_;                // Form: an error (red)
    std::uint64_t freeBytes_ = 0;
    LifCreator creator_;
    int lastPercent_ = -1;
    std::string created_;
    int flashKey_ = -1;                  // a key drawn pressed, until flashUntil_
    std::uint32_t flashUntil_ = 0;
    bool flashTimed_ = false;

    // Layout
    Rect frame_;
    int cx_ = 0, cw_ = 0, ty_ = 0, rowPitch_ = 0, infoY_ = 0, msgY_ = 0;
    Rect nameBox_, typeL_, typeR_, dirL_, dirR_, cancelBtn_, createBtn_, progress_;
    Rect field_;
    std::vector<Key> keys_;
};

}  // namespace hipi
