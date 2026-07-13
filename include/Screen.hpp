// SPDX-License-Identifier: MIT
//
// HP82163 video-display emulator — Screen class
// Ported from MicroPython to C++17.
//
// Original MicroPython code: J. Chilla, March 2026.
// This port targets a C++ program driving an RA8875 LCD.
//
// Usage (sketch):
//
//   #include "RA8875.hpp"
//   #include "Screen.hpp"
//   hp82163::MySpiTransport t;        // your platform-specific transport
//   hp82163::RA8875        display(t);
//   display.begin();                  // initialise display + upload CGRAM font
//   hp82163::Screen screen(display, /*color=*/0xFFFF, /*size=*/0, /*brightness=*/200);
//   for (uint8_t c : "Hello, world!")  screen.pr_char(c);
//
// The Screen class consumes HP82163 (HP-IL HP-41 video) byte streams and
// renders them on the RA8875 display.  See README.md for details.

#pragma once

#include "RA8875.hpp"
#include <cstdint>
#include <vector>

namespace hp82163 {

class Screen {
public:
    // Construct a Screen backed by an already-initialised RA8875.
    //
    //   display    — RA8875 driver (must outlive the Screen)
    //   color      — foreground text colour (RGB565)
    //   size       — text size 0..3 (built-in CGRAM modes) or 4 (custom 10x20 "fon" mode)
    //   brightness — 0..255 backlight duty cycle
    //
    //  The display must be put in 8BPP / 2-layer config and the CGRAM font
    //  must be uploaded *before* constructing the Screen.  See
    //  RA8875::begin() and the README.
    Screen(RA8875& display,
        std::uint16_t color,
        std::uint8_t size,
        std::uint8_t brightness,
        std::uint16_t textWidth = 800);

    // Process a single HP82163 input byte.  This is the main entry point
    // for an HP-41 display stream.  Recognised bytes include ASCII printable
    // characters, BS, LF, CR, ESC, and the HP82163 escape sequences.
    void pr_char(std::uint8_t c);

    // ----- High-level commands used by pr_char but also useful externally ---

    // Clear the screen and reset cursor + counters.
    void clear();

    // Redraw all currently-visible lines from the line buffer.
    // Useful after changing size or restoring the layer.
    void full();

    // Suspend/resume screen output. While suspended, pr_char() keeps
    // updating the internal line buffer exactly as normal, but stops
    // touching the actual display -- so something else (e.g. a UiDialog)
    // can draw over the screen without incoming HP-41 text disturbing it.
    // resume() re-enables output and immediately does a full() redraw to
    // catch up on anything that was written while suspended.
    void suspend()  { suspended_ = true; }
    void resume()   { suspended_ = false; full(); }
    bool isSuspended() const { return suspended_; }

    // Scroll / roll helpers matching the HP82163 commands.
    //   roll=true  -> also draw the new bottom line from the buffer
    //   cmd=true   -> also rotate the line buffer (true for ESC-83/84)
    void up(bool roll = false, bool cmd = true);
    void down(bool cmd = true);

    // Layer-2 helpers (the RA8875 is configured for 2 layers in begin()).
    void store();    // copy visible layer -> layer 2
    void recall();   // copy layer 2 -> visible layer

    // Recompute layout parameters for a given text size (0..4).
    void screen_pars(std::uint8_t size);

    // Change text size (0..3 -> built-in CGRAM, 4 -> custom "fon" mode).
    void txt_size(std::uint8_t size);

    // ----- Accessors -----
    std::uint8_t rows()    const { return ROWS_; }
    std::uint8_t cols()    const { return COLS_; }
    std::uint8_t row()     const { return row_; }
    std::uint8_t col()     const { return col_; }
    std::uint16_t width()  const { return width_; }
    std::uint16_t height() const { return height_; }
    std::uint8_t size()    const { return size_; }
    std::uint16_t color()  const { return color_; }

    // Change the screen's own foreground text color (persisted -- survives
    // a later full() redraw, unlike writing txtColor() on the RA8875
    // directly, which full() will now override on every redraw).
    void setColor(std::uint16_t c) { color_ = c; d_.txtColor(color_, 0); }

    // Cursor-mode commands (HP82163 ESC-60/62, 81/82, 65..68, 72, 37).
    void cursor(std::uint8_t cur);

    // ESC % row col: direct cursor addressing (matches HP82163 protocol).
    //   row, col — 1-based byte values from the HP-41 stream.
    void cursor_pos(std::uint8_t row, std::uint8_t col);

    // Insert character (used internally when in ESC N insert mode).
    void inschar();

private:
    // ---- Internal cursor helpers ----
    void set_cursor(std::uint8_t c, std::uint8_t r);
    void set_cur();
    void draw_letter(std::uint8_t c);
    void fon_write(const char* s);

    // Forwards to d_.BTE(), but does nothing while suspended_ is true.
    // All Screen-internal BTE calls go through this single choke point.
    void bte(std::uint8_t opcode,
             std::uint16_t x1, std::uint16_t y1,
             std::uint16_t w, std::uint16_t h,
             std::uint16_t x0 = 0, std::uint16_t y0 = 0);

    // ---- Mode switch ----
    void fon_mode();

    // ---- Members ----
    RA8875& d_;

    std::uint16_t color_;
    std::uint8_t size_;
    std::uint8_t ROWS_;
    std::uint8_t COLS_;
    std::uint16_t width_;
    std::uint16_t height_;
    std::uint8_t ofx_;
    std::uint8_t ofy_;

    std::uint8_t row_;
    std::uint8_t col_;

    std::uint16_t textWidth_ = 800;

    // State for escape-sequence parsing
    bool flag_;
    bool nline_;
    bool escN_;
    bool Ins_;      // insert-mode toggle (full block vs. underscore cursor)
    bool cv_;       // cursor-visible toggle
    int  n_;        // ESC % sequence state: -1=just ESC, 0=awaiting row, 1=awaiting col
    std::uint8_t pos_[2];  // ESC % row/col coordinates

    // Line buffer for scroll-back (one virtual line per screen line; Max + ROWS).
    std::vector<std::vector<std::uint8_t>> lines_;
    std::size_t offset_;     // top-of-screen offset in lines_ (used by up/down(cmd=false))
    std::size_t max_;        // soft cap on lines_.size()

    std::uint8_t cnt_;       // chars in current physical line
    std::uint8_t cp_;        // cursor position within current physical line

    bool suspended_ = false; // true while a UI dialog owns the display
};

}  // namespace hp82163