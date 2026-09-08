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
//   #include "display_config.h"   // selects RA8875 or LT7683 -- see its own comment
//   #include "Screen.hpp"
//   hipi::MySpiTransport t;        // your platform-specific transport
//   hipi::DisplayDriver  display(t);
//   display.begin();                  // initialise display + upload CGRAM font
//   hipi::Screen screen(display, /*color=*/0xFFFF, /*size=*/0, /*brightness=*/200);
//   for (uint8_t c : "Hello, world!")  screen.pr_char(c);
//
// The Screen class consumes HP82163 (HP-IL HP-41 video) byte streams and
// renders them on the display. See README.md for details.

#pragma once

#include "display_config.h"
#include <cstdint>
#include <vector>

namespace hipi {

class Screen {
public:
    // Construct a Screen backed by an already-initialised display driver.
    //
    //   display    — display driver (must outlive the Screen)
    //   color      — foreground text colour (RGB565)
    //   size       — text size 0..3 (built-in CGRAM modes) or 4 (custom 10x20 "fon" mode)
    //   brightness — 0..255 backlight duty cycle
    //
    //  The display must be put in 8BPP / 2-layer config and the CGRAM font
    //  must be uploaded *before* constructing the Screen.  See
    //  DisplayDriver::begin() and the README.
    Screen(DisplayDriver* display,
        std::uint16_t color,
        std::uint8_t size,
        std::uint8_t brightness,
        std::uint16_t textWidth = 800);

    // Process a single HP82163 input byte.  This is the main entry point
    // for an HP-41 display stream.  Recognised bytes include ASCII printable
    // characters, BS, LF, CR, ESC, and the HP82163 escape sequences.
    void pr_char(std::uint8_t c);
    void pr_str(const char *p);

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
    void resume()   { suspended_ = false; txt_size(size_); full(); }
    bool isSuspended() const { return suspended_; }

    // Explicitly turns off the hardware cursor (MWCR0's visible/blink
    // bits) without touching cv_ (the user's own cursor-visible HP-41
    // stream setting) -- suspend() alone only stops OUR OWN future draws;
    // it doesn't touch whatever cursor state the RA8875 already had
    // active, which otherwise just keeps blinking autonomously via the
    // chip's own hardware timer regardless of software suspension. Used
    // when an alternate full-screen view (see plotterview.h) takes over
    // the panel. resume()'s full() call (which ends with set_cur())
    // correctly restores the cursor afterward, based on the unchanged
    // cv_, so this doesn't need an explicit "undo".
    void hideCursorHardware() {
        d_->writeReg(0x40, 0x82);  // text mode, invisible cursor, auto-increment off
    }

    // Re-asserts the cursor's visibility/style/position. Call this after
    // anything OUTSIDE Screen touches the shared RA8875 in a way that might
    // have clobbered the cursor-visible bit in MWCR0 -- e.g. gfxMode()
    // (used internally by drawBitmap565()/drawBitmap332(), which the
    // button press-feedback redraw in pico_main.cpp calls directly on the
    // display, bypassing Screen entirely) blindly zeros that register.
    void refreshCursor() { set_cur(); }

    // Manual scroll-back, independent of the HP-41 stream's own animated
    // roll/paper-feed commands (up()/down() above). Positive n moves
    // further into history (older content); negative moves back toward
    // the live view (newer content). Clamped to the buffered range.
    // Redraws once via full() if the offset actually changed.
    void scrollBy(int n);

    // Jump straight back to the live/current view (offset = 0).
    void scrollToLive();

    // Scroll / roll helpers matching the HP82163 commands.
    //   roll=true  -> also draw the new bottom line from the buffer
    //   cmd=true   -> also rotate the line buffer (true for ESC-83/84)
    void up(bool roll = false, bool cmd = true);
    void down(bool cmd = true);

    // Layer-2 helpers (the RA8875 is configured for 2 layers in begin()).
    // store()/recall() removed -- were BTE-based off-screen staging
    // helpers only used by the old inschar() implementation, which no
    // longer needs them (see its own comment in Screen.cpp).

    // Recompute layout parameters for a given text size (0..4).
    void screen_pars(std::uint8_t size);

    // Adjusts every buffered line to the current COLS_ (padding with spaces
    // if wider, truncating if narrower) and ensures there are enough lines
    // to fill the current ROWS_, instead of wiping the buffer outright.
    // Used by setTextSize()/setColumns() so changing font size or column
    // count preserves existing text.
    void reflow();

    // Change text size (0..3 -> built-in CGRAM, 4 -> custom "fon" mode).
    void txt_size(std::uint8_t size);

    // ----- Accessors -----
    std::uint8_t rows()    const { return ROWS_; }
    std::uint8_t cols()    const { return COLS_; }
    std::uint8_t row()     const { return row_; }
    std::uint8_t col()     const { return col_; }
    // Whether an ESC sequence is currently mid-parse (waiting for more
    // bytes) -- exposed for boardui.cpp's own diagnostic logging around
    // showButtonStrip(), since Screen.cpp itself can't use LOGF (it's
    // part of the platform-neutral hipi_core library, no TinyUSB link).
    bool escInProgress()   const { return escN_; }
    // Diagnostic accessors -- see boardui.cpp's own unconditional
    // per-tap state dump for why these are here (testing whether row_/
    // col_/numLines_/offset_ ever drift out of their expected bounds,
    // which would explain text landing off-screen while the cursor
    // register write itself still "succeeds").
    std::size_t numLines() const { return numLines_; }
    std::size_t scrollOffset() const { return offset_; }
    // Running count of characters actually drawn via draw_letter() --
    // same reasoning as escInProgress() for why this is here instead of
    // a direct LOGF call. Lets pico_main.cpp's own periodic diagnostic
    // compare this against how many characters actually arrived over
    // HP-IL (CDisplay::doListener()'s own "Push" log), to see whether
    // drawing silently falls behind/stops relative to reception.
    std::uint32_t charsDrawn() const { return charsDrawn_; }
    std::uint16_t width()  const { return width_; }
    std::uint16_t height() const { return height_; }
    std::uint8_t size()    const { return size_; }
    std::uint16_t color()  const { return color_; }
    std::uint8_t brightness() const { return brightness_; }

    // Explicit column count, overriding the auto-computed max for the
    // current font size (e.g. to reproduce the original HP82163's 32
    // columns regardless of font size). 0 means "auto" (use the max the
    // current font size and textWidth_ allow). Clamped silently if the
    // requested value exceeds that max. Recomputes layout and clears, same
    // as setTextSize().
    std::uint8_t columnsOverride() const { return columnsOverride_; }
    void setColumns(std::uint8_t cols) {
        columnsOverride_ = cols;
        screen_pars(size_);
        reflow();
    }
    // Max columns available at the current font size (before any override).
    std::uint8_t maxColumns() const {
        return static_cast<std::uint8_t>(textWidth_ / width_);
    }

    // Usable pixel width for text (e.g. narrower while the button strip is
    // shown, full panel width while it's hidden). Recomputes layout and
    // reflows existing text to the new width instead of clearing it --
    // same pattern as setColumns()/setTextSize(). Caller is responsible for
    // also adjusting the RA8875's own active window to match (Screen has
    // no knowledge of where the button strip lives on screen).
    std::uint16_t textWidth() const { return textWidth_; }
    void setTextWidth(std::uint16_t w) {
        textWidth_ = w;
        screen_pars(size_);
        reflow();
    }

    // Change the screen's own foreground text color (persisted -- survives
    // a later full() redraw, unlike writing txtColor() on the RA8875
    // directly, which full() will now override on every redraw).
    void setColor(std::uint16_t c) { color_ = c; d_->txtColor(color_, 0); }

    // Change the backlight duty cycle (0..255, straight through to the
    // RA8875's PWM1 register -- see RA8875::brightness()).
    void setBrightness(std::uint8_t level) { brightness_ = level; d_->brightness(level); }

    // Change text size at runtime (0..3 -> built-in CGRAM modes). This
    // recomputes the row/column layout and clears the screen, same as
    // the HP82163 ESC [ / ESC ] stream commands do internally.
    // reflow() (not clear()) -- preserves existing buffer content,
    // reformatted to the new font's row/column layout, rather than
    // wiping it. The original "old text flashes at the new size" symptom
    // this used to work around wasn't actually about stale content --
    // it was full() drawing without checking suspended_ (see full()'s
    // own comment, now fixed centrally there), so an ESC sequence
    // arriving from HP-IL while the dialog was still open could trigger
    // an extra, premature redraw. With that fixed, reflow() here is
    // correct: setTextWidth() below uses the same approach for the same
    // reason (e.g. the button strip narrowing/widening the text area).
    void setTextSize(std::uint8_t size) { screen_pars(size); reflow(); }

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

    // bte() removed -- every internal call site was converted to redraw
    // affected rows from lines_ instead; see Screen.cpp's own comment
    // (up() specifically) for why.

    // ---- Mode switch ----
    void fon_mode();

    // ---- Members ----
    DisplayDriver* d_;

    std::uint16_t color_;
    std::uint8_t brightness_;
    std::uint8_t size_;
    std::uint8_t columnsOverride_ = 0;   // 0 = auto (max for current font size)
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
    std::uint32_t charsDrawn_ = 0;  // see charsDrawn()'s own comment
    bool Ins_;      // insert-mode toggle (full block vs. underscore cursor)
    bool cv_;       // cursor-visible toggle
    int  n_;        // ESC % sequence state: -1=just ESC, 0=awaiting row, 1=awaiting col
    std::uint8_t pos_[2];  // ESC % row/col coordinates

    // Line buffer for scroll-back -- a SINGLE, FIXED-SIZE flat allocation
    // (max_ * colsCapacity_ bytes, sized once and never grown/shrunk
    // during scrolling), not the previous std::vector<std::vector<uint8_t>>
    // (one separate heap allocation per line). That design meant every
    // single scroll event allocated a fresh line and freed the oldest one
    // -- confirmed on real hardware that this fragments the heap on this
    // embedded platform badly enough to crash after only ~80-115 lines
    // (varying between boots -- the classic signature of fragmentation,
    // not a fixed/deterministic limit), independent of how high max_
    // itself was set. A flat buffer makes ordinary scrolling (see up()'s
    // own comment) a single memmove -- no allocation at all, ever, after
    // construction. Matches the HP82163 Owner's Manual's own scope for
    // this (31 lines of buffered history) rather than trying to keep
    // hundreds of lines of scroll-back, which was never actually needed.
    //
    // rowPtr(i) returns a pointer to logical row i's colsCapacity_ bytes
    // (i=0 is the newest row, matching the old lines_[0] convention).
    std::vector<std::uint8_t> lines_;
    std::uint16_t colsCapacity_;  // bytes reserved per row in lines_ (>= any COLS_ this
                                    // Screen will ever use, computed once in screen_pars())
    std::size_t numLines_;    // how many of max_ rows are actually populated (grows
                                // from ROWS_ up to max_, then stays there)
    // One row's worth of reusable scratch space -- only needed for ESC-S's
    // top-around rotation (see up()'s own comment), which is relatively
    // rare (user-triggered, not per-character) but still shouldn't
    // allocate fresh every time it's used. Sized/filled alongside lines_
    // in screen_pars().
    std::vector<std::uint8_t> scratch_;
    std::uint8_t* rowPtr(std::size_t logicalIdx) {
        return lines_.data() + logicalIdx * colsCapacity_;
    }
    const std::uint8_t* rowPtr(std::size_t logicalIdx) const {
        return lines_.data() + logicalIdx * colsCapacity_;
    }
    std::size_t offset_;     // top-of-screen offset in lines_ (used by up/down(cmd=false))
    std::size_t max_;        // row capacity of lines_ -- computed once in the
                              // constructor from the panel's actual height (must be
                              // >= the largest possible ROWS_, see the constructor's
                              // own comment for why a fixed "31" wasn't safe)

    std::uint8_t cnt_;       // chars in current physical line
    std::uint8_t cp_;        // cursor position within current physical line

    bool suspended_ = false; // true while a UI dialog owns the display
};

}  // namespace hipi