#pragma once
// video80.h -- the HP 92198 / Mountain Computer MC00701A HP-IL 80-Column
// Video Interface: its screen memory, cursor and escape sequences, and
// its view on the panel. TFDISPLAY (display.h) feeds it when it emulates
// the HP 92198 (the display model setting); otherwise Screen is used.
//
// The model, after the owner's manual (Appendix A, C):
//   - 48 lines of screen memory, 80 (or 40) columns; a window of 24 (or 20)
//     lines of it is shown. The cursor is always in the window: ESC % and
//     the cursor keys address the screen, not the memory.
//   - CR, LF, BS and ESC are acted on; other control codes are ignored --
//     or shown, in monitor mode (ESC Y .. ESC Z).
//   - LF on the last line of the window: if the window isn't at the end of
//     the memory it moves down a line and the line coming into view is
//     cleared; at the end of the memory everything moves up and the first
//     line is lost. ESC S / ESC T move the window down / up.
//   - Lines longer than the screen continue on the next line (an implicit
//     CR LF). Such a "long line" is edited as one: inserted characters push
//     on into the next line (a new line is inserted for them if needed),
//     deleted ones pull the rest back.
//   - Character sets: ASCII, where 128..255 are the same characters in
//     inverse video, or Roman-8 (ASCII + the HP Roman extension, no
//     inverse). ESC j / ESC k switch; the whole screen follows.
//   - Status: $80, then (bit 0) 40 columns, (1) Roman-8, (2) PAL, (3)
//     cursor off, (4) insert mode.
//
// On the panel: the chip's own text engine, built-in ISO 8859-1 font --
// 12x24 on the 7" panel (80 x 24 = 960 x 576 px) unless the small one is
// chosen, 8x16 on the 5"; the 40-column mode doubles the width. Roman-8 is mapped to the nearest
// Latin-1 character. The view only draws while it's shown (setActive());
// otherwise only the memory changes, and showing it draws it all.
//
// HIPI's own "ESC # <c> [<arg>]" (screen dumps, choosing the view -- see
// Screen::setExtCommandCallback()) works here too.

#include "display_config.h"
#include <cstdint>
#include <functional>

namespace hipi {

class Video80 {
public:
    static constexpr int kBufLines = 48;      // screen memory
    static constexpr int kMaxCols = 80;

    explicit Video80(DisplayDriver* d);

    // One byte from the controller
    void put(std::uint8_t c);

    // DCL / SDC, and ESC e: memory cleared, cursor home, defaults
    void hardReset();

    // Settings: the small 8x16 font instead of 12x24 (the 5" panel only
    // has the small one), and 40 x 20 after a reset instead of 80 x 24
    // (set now too -- the screen is cleared)
    void setSmallFont(bool small);
    bool smallFont() const { return small_; }
    void setDefaultForty(bool forty);
    bool defaultForty() const { return defaultForty_; }

    // ── The view ──────────────────────────────────────────────────────
    // Shown (draws everything now and keeps drawing) or not (only the
    // memory changes; the cursor is hidden)
    void setActive(bool on);
    bool active() const { return active_; }
    // Draws everything again (when shown)
    void drawAll();
    // Draws everything if something was held back (setDrawGate()), and
    // puts the cursor back in any case: other drawing (the menu, the
    // button strip, info boxes) moves the chip's one shared text cursor
    void catchUp();
    // Draws again what lies in the screen region x/y/w/h (e.g. where the
    // button strip was, on the 5" panel)
    void drawRegion(int x, int y, int w, int h);
    // Drawing can be held off (e.g. while the 5" panel's menu covers the
    // screen); a held-off change makes the next drawAll() needed
    void setDrawGate(std::function<bool()> mayDraw) { mayDraw_ = std::move(mayDraw); }
    // The blinking cursor can be held hidden while the text still draws
    // (the 7" menu: its own text moves the chip's one shared text cursor)
    void setCursorGate(std::function<bool()> mayShow) { cursorMayShow_ = std::move(mayShow); }
    // Looking back through the memory (the arrow keys): n lines towards
    // the top (n > 0) or the end (n < 0) of the memory. live() follows
    // the window again.
    void scrollView(int n);
    void viewLive();
    bool viewIsLive() const { return live_; }
    // Text colour (the background is black)
    void setColor(std::uint16_t fg);
    void setExtCommandCallback(std::function<void(std::uint8_t, std::uint8_t)> cb,
                               const char* argCmds = "") {
        extCommand_ = std::move(cb);
        extArgCmds_ = argCmds != nullptr ? argCmds : "";
    }

    // ── State (status, tests) ─────────────────────────────────────────
    std::uint8_t status() const;          // the device dependent status byte
    int cols() const { return cols_; }
    int rows() const { return rows_; }
    int windowTop() const { return top_; }
    int cursorRow() const { return cr_; }      // in the window
    int cursorCol() const { return cc_; }
    bool cursorOn() const { return cursorOn_; }
    bool insertMode() const { return insert_; }
    bool roman8() const { return roman8_; }
    bool monitorMode() const { return monitor_; }
    std::uint8_t cell(int line, int col) const { return buf_[line][col]; }
    bool continued(int line) const { return cont_[line]; }
    // A memory line as text (spaces kept), for tests and the log
    void lineText(int line, char* out /* cols()+1 */) const;
    // The screen geometry (panel pixels)
    int originX() const { return ox_; }
    int originY() const { return oy_; }
    int cellW() const { return cw_; }
    int cellH() const { return ch_; }

private:
    // ── Model ─────────────────────────────────────────────────────────
    void control(std::uint8_t c);           // CR LF BS
    void escape(std::uint8_t c);            // ESC <c>
    void printable(std::uint8_t c);
    void lineFeed();
    void clearLine(int line, int from = 0);
    void insertLineAt(int line);            // memory lines from `line` down one
    void deleteLineAt(int line);
    void insertCharAt(int line, int col, std::uint8_t c, int depth = 0);
    void deleteCharAt(int line, int col, int depth = 0);
    void setWidth(bool forty);
    void clearAll();
    int line() const { return top_ + cr_; }  // the cursor's memory line

    // ── Drawing ───────────────────────────────────────────────────────
    bool canDraw() const;
    int viewTop() const { return live_ ? top_ : viewTop_; }
    void prepareText();
    void drawRow(int screenRow);
    void drawCell(int line, int col);
    void drawLines(int fromLine, int toLine);   // memory lines, those in view
    void scrolledUp();                       // the view's content moved up a line
    void placeCursor();
    void glyph(std::uint8_t b, std::uint8_t& code, bool& inverse, bool& special) const;
    void geometry();

    DisplayDriver* d_;
    std::uint8_t buf_[kBufLines][kMaxCols];
    bool cont_[kBufLines];                  // the line continues the one above
    int cols_ = 80, rows_ = 24;
    int top_ = 0;                           // the window's first memory line
    int cr_ = 0, cc_ = 0;                   // cursor, in the window
    bool wrapPending_ = false;              // the last column was just written
    bool cursorOn_ = true, insert_ = false, insertCursor_ = false;
    bool monitor_ = false, roman8_ = false, pal_ = false;

    // Escape sequence parsing
    enum class Esc : std::uint8_t { None, Esc, PosCol, PosRow, Ext, ExtArg, MonEsc };
    Esc esc_ = Esc::None;
    std::uint8_t posCol_ = 0;
    std::uint8_t extCmd_ = 0;
    std::function<void(std::uint8_t, std::uint8_t)> extCommand_;
    const char* extArgCmds_ = "";

    // View
    bool active_ = false;
    bool live_ = true;
    int viewTop_ = 0;
    bool needAll_ = true;                   // something wasn't drawn
    std::function<bool()> mayDraw_;
    std::function<bool()> cursorMayShow_;
    std::uint16_t fg_ = 0xFFFF, bg_ = 0x0000;
    int ox_ = 0, oy_ = 0, cw_ = 8, ch_ = 16; // screen origin, cell size (pixels)
    bool small_ = false, defaultForty_ = false;
    std::uint8_t fontCode_ = 1;             // CCR0 bits 5:4
    int glyphW_ = 0, glyphH_ = 0;            // the font's cell (0: not chosen yet)
};

}  // namespace hipi
