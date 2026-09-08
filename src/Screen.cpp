// SPDX-License-Identifier: MIT
//
// HP82163 video-display emulator — Screen class.
// C++17 port of the MicroPython class by J. Chilla, March 2026.

#include "Screen.hpp"

#include <algorithm>
#include <cstring>

namespace hipi {

// -----------------------------------------------------------------------
// Construction
// -----------------------------------------------------------------------

Screen::Screen(DisplayDriver* display,
               std::uint16_t color,
               std::uint8_t size,
               std::uint8_t brightness,
               std::uint16_t textWidth)
    : d_(display),
      color_(color),
      brightness_(brightness),
      size_(size),
      textWidth_(textWidth),
      ROWS_(0),
      COLS_(0),
      width_(0),
      height_(0),
      ofx_(0),
      ofy_(0),
      row_(0),
      col_(0),
      flag_(false),
      nline_(false),
      escN_(false),
      Ins_(false),
      cv_(true),
      n_(-1),
      pos_{0, 0},
      lines_(),
      colsCapacity_(0),
      numLines_(0),
      offset_(0),
      max_(0),  // computed properly in the constructor body below, once
                 // d_ (needed for d_->height()) is available -- see there
                 // for why this can't just be a fixed constant like the
                 // manual's own "31 lines" figure.
      cnt_(0),
      cp_(0) {
    // max_ MUST be at least as large as the largest ROWS_ this Screen
    // could ever need (the smallest font size, size=0, needs the most
    // rows to fill the panel) -- otherwise the flat buffer in lines_ is
    // too small to even hold one full screen's worth of rows, and
    // anything indexing past max_ reads/writes out of bounds. Confirmed
    // on real hardware: max_ fixed at 31 (matching the HP82163 Owner's
    // Manual's own scroll-back depth, reasonable in general) while this
    // board's actual ROWS_ at size=0 is 37 (600px panel / 16px per row)
    // corrupted exactly the 6 rows past the buffer's end -- matching the
    // "first ~6 rows go blank" symptom precisely. The manual's own "31
    // lines" was sized for its own small, fixed-resolution CRT display,
    // which never had to accommodate a taller panel or a smaller font
    // needing more physical rows than that.
    constexpr std::uint16_t kMinHeightPerRow = 16;  // height_ at size=0 (the smallest, most-rows case)
    const std::uint8_t maxPossibleRows =
        static_cast<std::uint8_t>(display->height() / kMinHeightPerRow);
    // A small amount of headroom above "exactly one full screen" for
    // genuine scroll-back history -- not the old, much larger (200, then
    // 60) caps that were never actually needed, just enough to page back
    // a few lines.
    max_ = static_cast<std::size_t>(maxPossibleRows) + 10;
    d_->txtColor(color, 0);
    d_->brightness(brightness);
    screen_pars(size);
    // Was writeReg(0x44, 0x0F) here directly (RA8875's CBLR, cursor
    // blink rate) -- REG[44h] is "Graphic Cursor Color 0" on LT7683, a
    // completely different register. Blink rate is cosmetic (both chips
    // still blink at their own default rate without this), so this is
    // simply dropped rather than adding another chip-specific
    // abstraction for it -- see setTextCursorVisible()'s own comment for
    // the cursor-visibility/style bug this project actually needed fixed.
    clear();
}

// -----------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------
void Screen::clear() {
    if (!suspended_) {
        d_->clearActiveWindow();
    }
    // Blank the whole fixed buffer (a single std::fill over the flat
    // array -- no allocation at all, unlike the old per-line vectors).
    std::fill(lines_.begin(), lines_.end(), 32);
    numLines_ = ROWS_;
    row_ = 0;
    col_ = 0;
    cnt_ = 0;
    cp_  = 0;
    escN_ = false;
    // Per the HP82163 spec: "Clears the display memory, moves the cursor
    // to home, and displays cursor as a blinking block" -- so Clear Device
    // always turns the cursor on (block style), regardless of whatever
    // cv_/Ins_ were left at before (e.g. from an earlier ESC < / ESC Q).
    cv_  = true;
    Ins_ = false;
    offset_ = 0;   // otherwise a later full() can index lines_ out of bounds
                    // if the buffer was cleared while scrolled back
    if (!suspended_) {
        // sleep(0.500) in the MicroPython original — keep a shorter delay here
        // since the MCLR_START is synchronous from the controller's perspective.
        d_->spiDelayMs(500);  // (declared below as a thin RA8875 helper)
    }
    set_cur();
}

void Screen::full() {
    // Centralised here so every caller (ESC J/K, ESC L, ESC M, ESC O,
    // resume(), etc.) automatically respects a currently-open UiDialog,
    // instead of each one needing to remember its own suspended_ check.
    // Previously several of those ESC-sequence handlers called this
    // unconditionally -- if HP-IL data triggering one of them arrived
    // while a dialog was still open, this would draw straight over it.
    // resume() itself still works correctly: it sets suspended_ = false
    // BEFORE calling this, so its own redraw goes through normally.
    if (suspended_) return;

    // Always restore our own foreground/background color before redrawing.
    // Something else (e.g. a UiDialog) may have changed the shared FG/BG
    // color registers on the display and never restored them.
    d_->txtColor(color_, 0);

    // ACTIVE, not FULL: full() should only redraw the text buffer within
    // the active window (set by setActiveWindow() in pico_main.cpp).
    // A full-screen clear would wipe out the button strip sitting outside
    // that window.
    d_->clearActiveWindow();
    // No extra delay needed here -- clearActiveWindow() already waits
    // synchronously for the geometry engine to finish internally (its own
    // rectHelper() call ends with waitPoll(), which blocks on the status
    // register until DCR0 reports done). A blind spiDelayMs(100) used to
    // sit here too, left over from when this went through BTE and its own
    // completion status wasn't trusted -- now that full() runs on every
    // single scroll event (not just occasionally), that redundant 100ms
    // was adding up to real, noticeable scroll lag on real hardware.
    // beginBulkTextDraw() BEFORE set_cursor() -- hides the cursor first,
    // so the position change right after doesn't cause a brief visible
    // jump to (0,0) before it's hidden (confirmed on real hardware: with
    // the reverse order, that jump was still visible even though the
    // subsequent per-row jumping was already fixed by hiding it at all).
    d_->beginBulkTextDraw();
    set_cursor(0, 0);
    for (int row = 0; row < ROWS_; ++row) {
        // Explicit per-row positioning: the RA8875's own auto-wrap kicks in
        // at the active window's full pixel width (i.e. the *max* columns
        // for the current font size), not our possibly-smaller COLS_ (set
        // via the Columns menu). Without this, a full repaint would wrap
        // at the hardware's width instead of ours.
        set_cursor(0, static_cast<std::uint8_t>(row));
        const std::size_t idx = static_cast<std::size_t>(ROWS_ - 1 - row + offset_);
        if (idx >= numLines_) continue;  // shouldn't happen; guards against it regardless
        const std::uint8_t* line = rowPtr(idx);
        for (std::uint8_t col = 0; col < COLS_; ++col) {
            draw_letter(line[col]);
        }
    }
    set_cur();  // re-asserts cursor visibility/style too, not just position
}

void Screen::up(bool roll, bool cmd) {
    if (!cmd && offset_ == 0) return;

    // Was per-scroll heap allocation (a fresh std::vector<uint8_t> line
    // created and the oldest one freed every single scroll event) --
    // replaced by a single memmove within the fixed-size flat buffer
    // (see lines_'s own comment for why: the old design fragmented the
    // heap badly enough to crash this embedded platform after only
    // ~80-115 lines, regardless of how high the old max_ cap was set).
    // No allocation happens here at all, ever, after construction.
    if (roll) {
        if (cmd) {
            // Top-around rotation (ESC-S): the last row wraps to become
            // the new first row, nothing is discarded or blanked.
            std::copy(rowPtr(numLines_ - 1), rowPtr(numLines_ - 1) + colsCapacity_,
                     scratch_.begin());
            std::memmove(rowPtr(1), rowPtr(0),
                        (numLines_ - 1) * static_cast<std::size_t>(colsCapacity_));
            std::copy(scratch_.begin(), scratch_.begin() + colsCapacity_, rowPtr(0));
        } else {
            offset_ = (offset_ > 0) ? offset_ - 1 : 0;
        }
    } else {
        if (cmd) {
            // Natural scroll from new output: grow numLines_ toward max_
            // first (if not there yet), THEN shift everything back by one
            // slot and blank row 0 -- at max_ already, the row that would
            // have landed past the end (the oldest) is simply never
            // copied anywhere, i.e. naturally evicted.
            const std::size_t usedAfter = (numLines_ < max_) ? numLines_ + 1 : max_;
            std::memmove(rowPtr(1), rowPtr(0),
                        (usedAfter - 1) * static_cast<std::size_t>(colsCapacity_));
            std::fill(rowPtr(0), rowPtr(0) + colsCapacity_, 32);
            numLines_ = usedAfter;
        } else {
            offset_ = (offset_ > 0) ? offset_ - 1 : 0;
        }
    }
    full();
}

void Screen::down(bool cmd) {
    if (!cmd && offset_ >= numLines_ - ROWS_) return;

    // Same flat-buffer, allocation-free approach as up() above -- see its
    // own comment. This rotates the opposite direction: row 0 (newest)
    // wraps around to become the last row.
    if (cmd) {
        std::copy(rowPtr(0), rowPtr(0) + colsCapacity_, scratch_.begin());
        std::memmove(rowPtr(0), rowPtr(1),
                    (numLines_ - 1) * static_cast<std::size_t>(colsCapacity_));
        std::copy(scratch_.begin(), scratch_.begin() + colsCapacity_, rowPtr(numLines_ - 1));
    } else {
        const std::size_t cap = numLines_ > ROWS_ ? numLines_ - ROWS_ : 0;
        offset_ = (offset_ + 1 <= cap) ? offset_ + 1 : cap;
    }
    full();
}

void Screen::scrollBy(int n) {
    if (numLines_ <= ROWS_) return;   // nothing buffered beyond one screen
    const long maxOffset = static_cast<long>(numLines_ - ROWS_);
    long newOffset = static_cast<long>(offset_) + n;
    if (newOffset < 0) newOffset = 0;
    if (newOffset > maxOffset) newOffset = maxOffset;
    if (static_cast<std::size_t>(newOffset) == offset_) return;

    const long actualDelta = newOffset - static_cast<long>(offset_);
    offset_ = static_cast<std::size_t>(newOffset);

    // Was two separate BTE block-move fast paths for actualDelta==+-1
    // (avoiding a full repaint for the common single-step case) -- same
    // BTE-avoidance fix as up()/down() above, for the same reason. Every
    // delta now just goes through full(), which was already the
    // multi-line-jump fallback below.
    full();
}

void Screen::scrollToLive() {
    if (offset_ == 0) return;
    offset_ = 0;
    full();
}

void Screen::inschar() {
    // Same flat-buffer approach as up()/down() -- operates directly on
    // this row's byte range within lines_ via memmove, instead of the
    // std::vector<uint8_t>::insert()/erase() the old per-line-vector
    // design used (see lines_'s own comment for why that mattered).
    std::uint8_t* line = rowPtr(ROWS_ - 1 - row_);
    const std::uint8_t last = line[COLS_ - 1];
    // Open a gap at col_: shift [col_, COLS_-1) right by one byte.
    std::memmove(line + col_ + 1, line + col_, COLS_ - 1 - col_);
    line[col_] = ' ';

    bool cascaded = false;
    if (!((cnt_ < COLS_) || (cp_ > COLS_))) {
        if ((cnt_ == COLS_) && (row_ == ROWS_ - 1)) {
            up();
            row_ = static_cast<std::uint8_t>(row_ - 1);
            line = rowPtr(ROWS_ - 1 - row_);  // up() shifted everything -- re-fetch
        }
        // The character pushed off the end of this row becomes the new
        // first character of the next row, which in turn pushes ITS own
        // last character further along -- same cascade the original
        // design implemented.
        std::uint8_t* next = rowPtr(ROWS_ - 2 - row_);
        std::memmove(next + 1, next, COLS_ - 1);
        next[0] = last;
        cascaded = true;
    }

    if (!suspended_) d_->beginBulkTextDraw();
    set_cursor(0, row_);
    for (std::uint8_t c = 0; c < COLS_; ++c) draw_letter(line[c]);
    if (cascaded) {
        set_cursor(0, static_cast<std::uint8_t>(row_ + 1));
        const std::uint8_t* next = rowPtr(ROWS_ - 2 - row_);
        for (std::uint8_t c = 0; c < COLS_; ++c) draw_letter(next[c]);
    }
    set_cur();  // re-asserts cursor visibility/style too, not just position

    cnt_ = static_cast<std::uint8_t>(cnt_ + 1);
}

void Screen::txt_size(std::uint8_t size) {
    if (suspended_) return;
    if (size < 4) {
        d_->txtSize(size);
        d_->writeReg(0x2E, 0);  // horizontal char spacing
        d_->writeReg(0x29, 0);  // vertical line spacing
    } else {
        d_->txtSize(0);
        fon_mode();
    }
}

void Screen::screen_pars(std::uint8_t size) {
    size_ = size;
    // Captured before being overwritten below -- needed by the
    // pre-scroll normalisation step further down (see its own comment).
    const std::uint8_t oldROWS = ROWS_;
    // Computed once, independent of the size being switched to: the
    // largest COLS_ this Screen could ever need across ANY font size
    // (the smallest character width, 8px at size=0, gives the most
    // columns). lines_ (see its own comment) is sized off this so it
    // never needs reallocating just because the font size changes later
    // -- allocated/grown here (screen_pars() only runs on construction or
    // an explicit size change, never per-scroll) rather than in clear()
    // or up(), which both run far more often.
    {
        const std::uint16_t maxPossibleCols = static_cast<std::uint16_t>(textWidth_ / 8);
        if (maxPossibleCols > colsCapacity_) {
            // FIXED: used to just lines_.assign(...) a fresh, all-blank
            // buffer at the new (wider) stride and reset numLines_ to 0
            // -- silently wiping every existing line of text. This path
            // isn't rare: hideButtonStrip() calls setTextWidth() with the
            // FULL panel width every time the button strip auto-hides
            // (see its own kButtonStripHideMs timeout in boardui.cpp),
            // which is wider than the narrower width Screen is
            // constructed with in pico_main.cpp (SCREEN_MAX_X minus the
            // button strip) -- so simply typing normally, with the strip
            // hiding itself in the background sometime after a font-size
            // or other menu change, was enough to trigger this and blank
            // the screen down to just whatever's typed after. Grows the
            // buffer's per-row stride while copying each existing row's
            // content into the new, wider layout instead -- the extra
            // columns each row gains are blank-padded, everything already
            // there is preserved, and numLines_ is untouched.
            const std::uint16_t oldColsCapacity = colsCapacity_;
            const std::vector<std::uint8_t> oldLines = std::move(lines_);
            colsCapacity_ = maxPossibleCols;
            lines_.assign(static_cast<std::size_t>(max_) * colsCapacity_, 32);
            if (oldColsCapacity > 0) {
                for (std::size_t row = 0; row < max_; ++row) {
                    const std::uint8_t* src = oldLines.data() + row * oldColsCapacity;
                    std::uint8_t* dst = lines_.data() + row * static_cast<std::size_t>(colsCapacity_);
                    std::copy(src, src + oldColsCapacity, dst);
                }
            }
            scratch_.assign(colsCapacity_, 32);
        }
    }
    if (size < 4) {
        const std::uint8_t k = static_cast<std::uint8_t>(1 + size);
        width_  = static_cast<std::uint16_t>(8 * k);
        const std::uint8_t maxCols = static_cast<std::uint8_t>(textWidth_ / width_);
        COLS_   = (columnsOverride_ > 0 && columnsOverride_ <= maxCols)
                      ? columnsOverride_ : maxCols;
        height_ = static_cast<std::uint16_t>(16 * k);
        // Was a flat 30/k -- correct for the original HP82163's 480px-tall
        // panel (15 rows * 32px = 480 at size=1), but hardcoded rather than
        // actually based on the real panel height. This board's own panel
        // is taller (600px) -- computed from d_->height() instead, the
        // same way COLS_ above is already computed from the real available
        // width (textWidth_) rather than a fixed column count. Confirmed
        // against the HP82163 Owner's Manual (uploaded to this
        // conversation) that ROWS_ itself has no fixed "must be N" spec to
        // preserve -- the manual's own "up to 16 lines" figure is
        // specific to ITS panel's fixed resolution, not a protocol
        // requirement; the escape-sequence/scrolling behaviour it
        // documents is defined in terms of "the display", not a literal
        // row count.
        ROWS_   = static_cast<std::uint8_t>(d_->height() / height_);
        ofx_ = 0;
        ofy_ = 0;
    } else {
        width_  = 10;
        COLS_   = static_cast<std::uint8_t>(textWidth_ / width_);   // <-- var: 80
        height_ = 20;
        ROWS_   = 24;
        ofx_ = 0;
        ofy_ = 3;
    }

    // Pre-scroll normalisation: content written since the last clear(),
    // BEFORE the screen has ever actually scrolled (up() never called --
    // numLines_ is still sitting at its post-clear() initial value of
    // oldROWS, not grown past it), was positioned using rowPtr(oldROWS-1
    // -row_) -- i.e. tied to the OLD ROWS_ that was in effect at write
    // time. full()'s own drawing loop (and up()'s own scroll logic)
    // both assume the OPPOSITE convention: buffer index 0 is always the
    // most-recently-written line, growing toward higher indices for
    // older ones, independent of ROWS_. Those only actually agree once
    // row_ has reached the bottom of a full screen and up() has shifted
    // things at least once -- until then, changing ROWS_ (a font-size
    // switch) left old content sitting at buffer positions that no
    // longer corresponded to what the new ROWS_-based draw loop expected
    // -- confirmed on real hardware: switching from a small font with
    // ~20 lines already typed (never having filled/scrolled that many
    // rows) to a larger font showed only the last couple of lines,
    // shoved to the top of the screen, with the cursor and new typing
    // landing far below with a gap of blank lines in between.
    //
    // Shifts the (row_ + 1) rows written so far -- everything from the
    // very first line down through the current, possibly still-partial
    // one -- so the most recent lands at index 0, matching the
    // convention every other consumer of this buffer already expects,
    // and updates numLines_ to the ACTUAL count of lines written (row_ +
    // 1), not the stale "as many as the old ROWS_" value clear() left
    // it at. row_ itself is left as-is: reflow()'s own existing clamp
    // (run right after this, back in setTextSize()/setTextWidth()) already
    // reduces it to (new ROWS_ - 1) if the new screen can't fit
    // everything, or leaves it alone otherwise -- exactly the right
    // behaviour either way once numLines_ is correct.
    if (oldROWS > 0 && numLines_ == oldROWS && row_ < oldROWS) {
        const std::size_t linesWritten = static_cast<std::size_t>(row_) + 1;
        const std::size_t srcIndex = static_cast<std::size_t>(oldROWS) - linesWritten;
        if (srcIndex > 0) {
            std::memmove(lines_.data(),
                        lines_.data() + srcIndex * static_cast<std::size_t>(colsCapacity_),
                        linesWritten * static_cast<std::size_t>(colsCapacity_));
        }
        // FIXED: the memmove above only ever overwrites the FIRST
        // linesWritten rows -- with overlapping src/dst ranges (moving
        // toward index 0), the tail of the original range, indices
        // [linesWritten, oldROWS), is left completely untouched, still
        // holding a stale copy of exactly the content that was just
        // moved. Harmless on its own (those indices are past numLines_,
        // which full()'s own drawing loop skips) -- until later, a
        // SUBSEQUENT reflow() (e.g. switching to yet another font size,
        // this time with a LARGER ROWS_) pads numLines_ back UP toward
        // that larger ROWS_ to show genuinely blank rows for the part
        // of the screen with no real content yet -- exposing these
        // stale leftovers instead of blank ones. Confirmed on real
        // hardware: switching between a couple of font sizes after
        // scrolling showed old content duplicated further down the
        // screen. Blanked explicitly here so there's nothing stale left
        // for a later reflow() to ever expose.
        if (linesWritten < static_cast<std::size_t>(oldROWS)) {
            std::fill(lines_.data() + linesWritten * static_cast<std::size_t>(colsCapacity_),
                     lines_.data() + static_cast<std::size_t>(oldROWS) * static_cast<std::size_t>(colsCapacity_),
                     32);
        }
        numLines_ = linesWritten;
    }

    txt_size(size);
}

void Screen::reflow() {
    // The flat buffer's rows are already colsCapacity_ bytes wide -- sized
    // for the largest COLS_ this Screen can ever use (see screen_pars()'s
    // own comment) -- so unlike the old per-line vectors, there's nothing
    // to grow here just because COLS_ changed. full() only ever draws the
    // first COLS_ bytes of each row regardless of how much more capacity
    // exists, so a narrower COLS_ just means the tail isn't drawn for
    // now -- it's not lost, and reappears once COLS_ grows again (e.g.
    // the button strip hiding widens the text area back out).
    //
    // Captured before being padded below -- see the row_ repositioning
    // fix further down for why this is needed.
    const std::size_t actualNumLines = numLines_;
    // Pad numLines_ up to ROWS_ if needed (ROWS_ depends on panel height,
    // not width, so this is mostly theoretical for a text-width-only
    // reflow -- but safe regardless: any not-yet-used rows are still
    // blank from screen_pars()'s own initial fill).
    if (numLines_ < ROWS_) numLines_ = ROWS_;
    // Clamp the scroll-back offset in case ROWS_ grew.
    if (numLines_ > ROWS_) {
        const std::size_t maxOffset = numLines_ - ROWS_;
        if (offset_ > maxOffset) offset_ = maxOffset;
    } else {
        offset_ = 0;
    }
    // Clamp the live cursor/line-tracking state to the new dimensions so
    // it doesn't point past the (possibly narrower/shorter) new layout.
    if (col_ >= COLS_) col_ = static_cast<std::uint8_t>(COLS_ > 0 ? COLS_ - 1 : 0);
    // FIXED: was just "if (row_ >= ROWS_) row_ = ROWS_ - 1" -- only ever
    // handles ROWS_ SHRINKING (row_ now overflows the smaller screen).
    // Switching to a LARGER ROWS_ (e.g. a smaller font, after the screen
    // had already scrolled at the previous, smaller ROWS_) left row_
    // completely unchanged at its old, smaller-screen-relative value --
    // confirmed on real hardware: the cursor (and all new typing) landed
    // stuck partway down the new, taller screen, with a gap of blank
    // rows both above AND below it, rather than moving to sit right
    // after the actual most-recent content the way it should. Only
    // meaningful for the "live" view (offset_ == 0) -- while scrolled
    // back into history (offset_ > 0), the cursor isn't visible on
    // screen at all regardless, so its exact row_ value doesn't matter
    // until the user scrolls back to live view anyway (which resets
    // offset_ to 0, at which point this same formula applies again
    // naturally on the next reflow()).
    if (offset_ == 0) {
        const std::size_t visibleLines = actualNumLines < ROWS_ ? actualNumLines : ROWS_;
        row_ = static_cast<std::uint8_t>(visibleLines > 0 ? visibleLines - 1 : 0);
    } else if (row_ >= ROWS_) {
        row_ = static_cast<std::uint8_t>(ROWS_ > 0 ? ROWS_ - 1 : 0);
    }
    if (cnt_ > COLS_) cnt_ = COLS_;
    if (cp_  > COLS_) cp_  = COLS_;

    // If a UiDialog is currently open (suspended_), don't draw over it --
    // just leave the buffer in its new shape. resume() will call full()
    // itself once the dialog closes, picking up the new layout then.
    if (!suspended_) full();
}

void Screen::cursor(std::uint8_t cur) {
    switch (cur) {
        case 65:  // ESC A - up
            if (row_ != 0) row_ = static_cast<std::uint8_t>(row_ - 1);
            break;
        case 66:  // ESC B - down
            if (row_ != ROWS_ - 1) row_ = static_cast<std::uint8_t>(row_ + 1);
            break;
        case 67:  // ESC C - right
            cp_ = static_cast<std::uint8_t>(cp_ + 1);
            if (col_ < COLS_ - 1) {
                col_ = static_cast<std::uint8_t>(col_ + 1);
            } else {
                col_ = 0;
                row_ = static_cast<std::uint8_t>(row_ + 1);
                if (row_ == ROWS_) row_ = 0;
            }
            break;
        case 68:  // ESC D - left
            cp_ = (cp_ > 0) ? static_cast<std::uint8_t>(cp_ - 1) : 0;
            if (col_ > 0) {
                col_ = static_cast<std::uint8_t>(col_ - 1);
            } else if (row_ > 0) {
                row_ = static_cast<std::uint8_t>(row_ - 1);
                col_ = static_cast<std::uint8_t>(COLS_ - 1);
            }
            break;
        case 72:  // ESC H - home
            col_ = 0;
            row_ = 0;
            break;
        default:
            // cursor-mode commands
            switch (cur) {
                case 60: cv_ = false; break;    // ESC < - Cursor off
                case 62: cv_ = true;  break;    // ESC > - Cursor on
                case 81:                        // ESC Q - Insert cursor
                    Ins_ = true;
                    break;
                case 82:                        // ESC R - Replace cursor
                    Ins_ = false;
                    escN_ = false;
                    break;
                default: return;
            }
            set_cur();
            return;
    }
    set_cur();  // re-asserts cursor visibility/style too, not just position
}

void Screen::cursor_pos(std::uint8_t row, std::uint8_t col) {
    // HP82163 spec: the cursor-address space is a FIXED 32-column x 16-row
    // grid ("column = m mod 32, row = n mod 16"), independent of our own
    // current COLS_/ROWS_ (which vary with font size). The original Python
    // used "% self.COLS" / "% self.ROWS" instead of the fixed 32/16 -- that
    // only happened to work if COLS_/ROWS_ were exactly 32/16 for whatever
    // size the original targeted; it's wrong in general.
    const std::uint8_t addrCol = static_cast<std::uint8_t>(col % 32);
    const std::uint8_t addrRow = static_cast<std::uint8_t>(row % 16);

    // Safety clamp: if the current font size gives fewer physical rows/
    // columns than the protocol's 32x16 address space allows, don't let an
    // otherwise-valid address write outside our own line buffers.
    col_ = (addrCol < COLS_) ? addrCol : static_cast<std::uint8_t>(COLS_ - 1);
    row_ = (addrRow < ROWS_) ? addrRow : static_cast<std::uint8_t>(ROWS_ - 1);
    set_cur();  // re-asserts cursor visibility/style too, not just position
}

// -----------------------------------------------------------------------
// pr_char — main HP82163 byte dispatcher
// -----------------------------------------------------------------------

void Screen::pr_char(std::uint8_t c) {
    if (flag_) {
        flag_ = false;
        if (n_ == 0) {
            pos_[0] = c;
            n_ = 1;
            flag_ = true;
        } else if (n_ == 1) {
            pos_[1] = c;
            // pos_[0] = column, pos_[1] = row (see cursor_pos()'s doc comment,
            // matching the HP-41 stream's byte order). The previous code
            // called cursor(37), which silently did nothing -- 37 isn't a
            // handled case in cursor()'s switch, so "ESC % row col" never
            // actually moved the cursor.
            cursor_pos(pos_[1], pos_[0]);
            n_ = -1;
        } else if (c == 37) {            // ESC %
            n_ = 0;
            flag_ = true;
        } else if (c == 83) {            // ESC S -> roll up
            up(true, true);
        } else if (c == 84) {            // ESC T -> roll down
            down(true);
        } else if (c == 65 || c == 66 || c == 67 || c == 68 ||
                   c == 72 || c == 81 || c == 82 || c == 60 || c == 62) {
            cursor(c);
        } else if (c == 69) {            // ESC E -> clear
            clear();
        } else if (c == 74 || c == 75) { // ESC J/K -> clear to EOL / EOS
            std::uint8_t* cur = rowPtr(ROWS_ - 1 - row_);
            std::fill(cur + col_, cur + COLS_, 32);
            if (c == 74) {
                for (std::uint8_t i = 0; i < ROWS_ - 1 - row_; ++i) {
                    std::fill(rowPtr(i), rowPtr(i) + COLS_, 32);
                }
            }
            full();
        } else if (c == 76) {            // ESC L -> insert line
            // Flat-buffer equivalent of the old vector insert-then-erase
            // (see lines_'s own comment for why that mattered): elements
            // [1, K) shift left to [0, K-1) (discarding what was newest,
            // at index 0), and a fresh blank row appears at K-1, where
            // K = ROWS_-1-row_. Everything from K onward is untouched.
            {
                const std::size_t K = ROWS_ - 1 - row_;
                if (K > 0) {
                    std::memmove(rowPtr(0), rowPtr(1), (K - 1) * static_cast<std::size_t>(colsCapacity_));
                    std::fill(rowPtr(K - 1), rowPtr(K - 1) + colsCapacity_, 32);
                }
            }
            col_ = 0;
            full();  // also re-asserts cursor visibility/style, via its own set_cur()
        } else if (c == 77) {            // ESC M -> delete line
            // Flat-buffer equivalent of the old vector erase-then-insert:
            // elements [0, K) shift right to [1, K+1) (discarding what was
            // at K), and a fresh blank row appears at index 0.
            {
                const std::size_t K = ROWS_ - 1 - row_;
                std::memmove(rowPtr(1), rowPtr(0), K * static_cast<std::size_t>(colsCapacity_));
                std::fill(rowPtr(0), rowPtr(0) + colsCapacity_, 32);
            }
            full();
        } else if (c == 78) {            // ESC N -> enter insert mode
            escN_ = true;
        } else if (c == 79) {            // ESC O -> delete character
            std::uint8_t* line = rowPtr(ROWS_ - 1 - row_);
            // Close the gap at col_: shift (col_, COLS_) left by one byte.
            std::memmove(line + col_, line + col_ + 1, COLS_ - 1 - col_);
            bool cascaded = false;
            if ((cnt_ < COLS_) || (cp_ > COLS_)) {
                line[COLS_ - 1] = 32;
            } else {
                std::uint8_t* next = rowPtr(ROWS_ - 2 - row_);
                line[COLS_ - 1] = next[0];
                std::memmove(next, next + 1, COLS_ - 1);
                next[COLS_ - 1] = 32;
                cascaded = true;
            }
            if (!suspended_) d_->beginBulkTextDraw();
            set_cursor(0, row_);
            for (std::uint8_t cc = 0; cc < COLS_; ++cc) draw_letter(line[cc]);
            if (cascaded) {
                set_cursor(0, static_cast<std::uint8_t>(row_ + 1));
                const std::uint8_t* next = rowPtr(ROWS_ - 2 - row_);
                for (std::uint8_t cc = 0; cc < COLS_; ++cc) draw_letter(next[cc]);
            }
            set_cur();  // re-asserts cursor visibility/style too, not just position
            if (cp_ != cnt_)
                cnt_ = (cnt_ > 0) ? cnt_ - 1 : 0;
        } else if (c == 91 || c == 93) {  // ESC [ / ] -> size 0/1
            screen_pars((c - 91) / 2);
            clear();
        }
    } else {
        if (c == 8 || c == 10 || c == 13 || c == 27) {
            if (c == 27) {
                nline_ = false;
                flag_ = true;
                n_ = -1;
            } else {
                if (c == 8) {
                    nline_ = false;
                    if (col_ != 0) {
                        col_--;
                    } else if (row_ != 0) {
                        row_--;
                        col_ = COLS_ - 1;
                    }
                } else if (c == 10) {
                    cnt_ = 0;
                    cp_  = 0;
                    if (!nline_) {
                        if (row_ == ROWS_ - 1)
                            up();
                        else
                            row_++;
                        nline_ = false;
                    }
                } else if (c == 13) {
                    col_ = 0;
                }
                set_cur();  // re-asserts cursor visibility/style too, not just position
            }
        } else {
            // printable character
            nline_ = false;
            if (escN_)
                inschar();
            rowPtr(ROWS_ - 1 - row_)[col_] = c;
            draw_letter(c);
            col_++;
            if (cp_ == cnt_)
                cnt_++;
            cp_++;
            if (col_ == COLS_) {
                col_ = 0;
                row_++;
                nline_ = true;
                if (row_ == ROWS_) {
                    row_ = ROWS_ - 1;
                    up();
                }
            }
            set_cur();  // re-asserts cursor visibility/style too, not just position
        }
    }
}

void Screen::pr_str(const char *p) {
    while( *p )
        pr_char(*p++);
    pr_char('\r');
    pr_char('\n');
}

// -----------------------------------------------------------------------
// Internals
// -----------------------------------------------------------------------

void Screen::set_cursor(std::uint8_t c, std::uint8_t r) {
    if (suspended_) return;
    // Was writeReg16(0x2A,...)/writeReg16(0x2C,...) directly -- correct
    // for RA8875 (cursor X/Y), but REG[2Ah]/[2Ch] mean something
    // completely different on LT7683 (PIP window screen position).
    // txtSetCursor() already exists specifically so callers don't need
    // to know which chip's registers to poke -- each driver's own
    // implementation targets its own correct address.
    d_->txtSetCursor(static_cast<std::uint16_t>(c * width() + ofx_),
                     static_cast<std::uint16_t>(r * height() + ofy_));
}

void Screen::set_cur() {
    if (suspended_) return;
    // Only actually show the cursor at the live position -- it sits on top
    // of whatever physical row_/col_ is, which is meaningless while
    // scrolled back into history (that row is showing an older line, not
    // where the next character would land).
    //
    // Was writeReg(0x40,...)/writeReg(0x4F,...) directly -- correct for
    // RA8875 (MWCR0 text-mode+cursor-visible bits, cursor-height
    // register), but on LT7683 those same two addresses are GCHP0
    // (Graphic Cursor Horizontal Position) and something else entirely,
    // not cursor visibility/style at all. setTextCursorVisible() exists
    // so each driver can target its own correct registers -- see its own
    // header comment for the full story.
    d_->setTextCursorVisible(cv_ && offset_ == 0, !Ins_);
    set_cursor(col_, row_);
}

// bte() (forwarded to d_->BTE()) removed -- every call site in this file
// has been converted to redraw the affected row(s) from lines_ instead
// (see up()'s own comment for the full story: BTE never got past
// triggering without freezing/corrupting the display on this hardware).
// Nothing else in the project called it either.

void Screen::draw_letter(std::uint8_t c) {
    if (suspended_) return;
    ++charsDrawn_;
    if (c > 127) {
        if (size_ < 4) {
            d_->txtColor(0, color_);
            d_->txtWriteChar(static_cast<std::uint8_t>(c & 0x7F));
        } else {
            d_->fillRect(static_cast<std::int16_t>(col_ * width()),
                        static_cast<std::int16_t>(row_ * height()),
                        static_cast<std::int16_t>(width()),
                        static_cast<std::int16_t>(height()),
                        color_);
            d_->txtColor(0, color_);
            const char tmp[2] = { static_cast<char>(c & 0x7F), 0 };
            fon_write(tmp);
        }
        d_->txtColor(color_, 0);
    } else {
        if (size_ < 4) {
            d_->txtWriteChar(c);
        } else {
            const char tmp[2] = { static_cast<char>(c), 0 };
            fon_write(tmp);
        }
    }
}

void Screen::fon_mode() {
//    if (d_->mode() != nullptr && std::strcmp(d_->mode(), "fon") == 0) return;
    if (suspended_) return;
    // KNOWN BUG, not yet fixed (this path -- size_>=4, "custom CGRAM
    // font" mode -- is currently unused; the project constructs Screen
    // with size=1 everywhere). Same class of issue already found and
    // fixed in txt_size() for size_<4: these four register addresses are
    // RA8875-era assumptions that mean something else entirely on
    // LT7683. Confirmed against the LT768x datasheet directly:
    //   REG[21h] is MISA[15:8] -- part of MAIN IMAGE START ADDRESS, not
    //     "FNCR0/CGRAM" at all. Writing 0x80 here would move Main Image
    //     Start Address away from 0 -- likely the same "screen goes black
    //     and stays black" symptom clearActiveWindow()'s old BTE bug had,
    //     not a harmless no-op.
    //   REG[2Eh]/REG[29h] are PISA (PIP image address) / MWULY[12:8]
    //     (Main Window Y high bits) -- see txt_size()'s own comment for
    //     the full story on these two.
    //   REG[40h] ("MWCR0: text mode" here) is GCHP0 (Graphic Cursor
    //     Horizontal Position) on LT7683, not a mode register.
    // Needs the same careful re-derivation txt_size() got before size=4
    // is usable on this chip -- don't just copy txt_size()'s fix
    // verbatim, these are a different set of registers.
    d_->writeReg(0x40, 0x80);  // MWCR0: text mode
    d_->writeReg(0x21, 0x80);  // FNCR0: CGRAM
    d_->writeReg(0x2E, 2);     // horizontal char spacing
    d_->writeReg(0x29, 4);     // vertical line spacing
    // We don't expose "fon" via RA8875::mode(); tag it with a no-op write.
    // (See note in Screen.hpp — we track mode implicitly via size_.)
}

void Screen::fon_write(const char* s) {
    if (suspended_) return;
    fon_mode();
    d_->beginMemoryWrite();
    for (const char* p = s; *p; ++p) {
        d_->writeData(static_cast<std::uint8_t>(*p));
        if (d_->txtScale() > 0) d_->spiDelayMs(1);
    }
}

// Special-case cursor command 37 ("ESC %" with row, col already in pos_)
namespace {
}  // namespace
}  // namespace hipi