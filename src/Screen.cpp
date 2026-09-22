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

            // BTE fast path: this is the single most frequent redraw this
            // whole class ever does (a natural scroll happens on every
            // new line once the screen's full, i.e. constantly during
            // normal HP-IL terminal use) -- full()'s own text-engine
            // redraw re-renders every character of every visible row from
            // scratch each time, even though ROWS_-1 of those rows'
            // pixels are already correctly on screen, just one row
            // higher than where they need to be. Shifts the RENDERED
            // pixels up by one row's height entirely via BTE (no SPI
            // pixel data at all -- see LT7683::bteScrollShift()'s own
            // comment for how this avoids the overlapping-copy risk),
            // then just blanks the newly-exposed bottom row -- the new
            // line's own text isn't drawn here at all, it arrives via
            // the normal, unchanged pr_char() path right after up()
            // returns, same as it always has.
            //
            // Only safe/meaningful in the live view (offset_ == 0) --
            // scrolled back into history, the newly-arrived line isn't
            // even necessarily visible on screen at all, and full()'s
            // own offset_-aware indexing already handles that case
            // correctly; not worth the complexity of teaching this fast
            // path the same logic when it's a comparatively rare case.
            if (!suspended_ && offset_ == 0 && bteScrollEnabled_) {
                const std::uint16_t pixelWidth =
                    static_cast<std::uint16_t>(COLS_ * width_);
                const std::uint16_t pixelHeight =
                    static_cast<std::uint16_t>(ROWS_ * height_);
                d_->bteScrollShift(0, 0, pixelWidth, pixelHeight, height_);
                d_->fillRect(0, static_cast<std::int16_t>(pixelHeight - height_),
                            pixelWidth, height_, 0x0000);
                // fillRect() ändrar foreground color.
                // Återställ Screen's textfärg innan pr_char() fortsätter skriva.
                d_->txtColor(color_, 0);
                return;
            }
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
        // Was writeReg(0x2E, 0)/writeReg(0x29, 0) directly -- WRONG
        // registers on LT7683 (see setCharSpacing()'s own comment,
        // LT7683.hpp, for the full story: those happen to be the
        // correct registers on RA8875, not LT7683, which is presumably
        // how this mistake happened). Kept here for correctness/future
        // use (e.g. any text path that relies on hardware auto-advance
        // instead of explicit positioning), but CONFIRMED on real
        // hardware to have NO VISIBLE EFFECT on draw_letter()'s own
        // output, at any value tried (including 10) -- draw_letter()'s
        // caller sets the cursor position explicitly before every
        // character (`col * width() + ofx_`), bypassing the hardware's
        // own auto-advance-plus-spacing mechanism entirely. The actual
        // character-gap fix is in screen_pars()'s own width_
        // calculation (this function's own caller) -- see its comment.
        d_->setCharSpacing(2);   // horizontal char spacing
        d_->setLineSpacing(0);   // vertical line spacing
    } else {
        d_->txtSize(0);
        fon_mode();
    }
}

void Screen::screen_pars(std::uint8_t size) {
    size_ = size;
    // Reset each call -- reflects only whether THIS call's own
    // normalisation block (further down) actually ran, not some earlier
    // call's. See its own declaration in Screen.hpp for why reflow()
    // needs this.
    preScrollNormalized_ = false;
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
        // Character cell width used for BOTH column count (textWidth_ /
        // width_) AND explicit per-character cursor positioning
        // (draw_letter()'s own caller does `col * width() + ofx_` --
        // see its own comment for why: the hardware's own auto-advance-
        // plus-F2FSSR-spacing mechanism is never used here, since the
        // cursor is explicitly repositioned before every character
        // regardless of whatever F2FSSR says to add). setCharSpacing()
        // (called from txt_size() below) therefore has NO visible
        // effect on ITS OWN -- confirmed on real hardware (tried up to
        // 10 pixels, no change) -- the actual fix is adding the gap
        // HERE, to the stride the cursor advances by, not to the
        // hardware register. +2 px per k: matches the requested 1-2px
        // gap at k=1 (size=0), scaled so it stays visually proportional
        // at larger sizes too (glyphs themselves scale by k).
        const std::uint8_t charGap = static_cast<std::uint8_t>(2 * k);
        width_  = static_cast<std::uint16_t>(8 * k + charGap);
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
    // time. When ROWS_ changes (a real font-size switch), that mapping
    // no longer lines up with the same rowPtr(ROWS_-1-row_) formula
    // every other consumer (full()'s own draw loop, and pr_char()'s own
    // write path for whatever gets typed next) now uses with the NEW
    // ROWS_ -- so the content has to be physically repositioned to match.
    //
    // FIXED (again): the previous version of this always moved content
    // to buffer index 0, on the theory that this matches "index 0 =
    // most recent", the convention up()'s own post-scroll shifting
    // uses. That's only actually correct when the moved content fills
    // the ENTIRE new screen (row_ + 1 >= new ROWS_) -- confirmed on real
    // hardware with exactly that case (~20 lines moving to an 18-row
    // screen) working fine. But for fewer lines than the new ROWS_ (e.g.
    // 4 lines, cursor still well short of a full screen), the correct
    // target position for the most-recently-written line is NOT index 0
    // -- it's (new ROWS_ - 1), matching reflow()'s own row_
    // repositioning right after this (row_ = min(linesWritten, ROWS_) -
    // 1) via the SAME rowPtr(ROWS_-1-row_) formula pr_char() itself
    // uses. Shifting to index 0 regardless left the cursor correctly
    // repositioned (a separate, correct computation) while the actual
    // TEXT ended up several rows away from it, with a gap of blank space
    // in between -- confirmed on real hardware: 4 lines typed at a
    // larger font, switched to a smaller font (fewer, taller rows), text
    // reappeared crammed at the bottom of the screen while the cursor
    // itself landed at the right row.
    //
    // keptLines: how many of the written lines actually fit in the new
    // ROWS_ -- if there are more written lines than the new, smaller
    // screen can show at once, the OLDEST ones are the ones that
    // scroll off (dropped here, not copied anywhere), matching normal
    // scroll behaviour.
    if (oldROWS > 0 && ROWS_ != oldROWS && numLines_ == oldROWS && row_ < oldROWS) {
        preScrollNormalized_ = true;
        const std::size_t linesWritten = static_cast<std::size_t>(row_) + 1;
        const std::size_t keptLines = std::min(linesWritten, static_cast<std::size_t>(ROWS_));
        const std::size_t srcIndex = static_cast<std::size_t>(oldROWS) - keptLines;
        const std::size_t dstIndex = static_cast<std::size_t>(ROWS_) - keptLines;
        const std::size_t stride = static_cast<std::size_t>(colsCapacity_);

        if (srcIndex != dstIndex) {
            // A plain memmove isn't safe here -- unlike the old "always
            // to index 0" version, src and dst can now be in either
            // order relative to each other (oldROWS vs ROWS_ can go
            // either way), so an in-place shift could clobber data it
            // still needs to read. Copies through a small scratch buffer
            // instead -- keptLines is at most a couple of dozen rows, so
            // this is cheap and only happens on an actual font-size
            // switch, not per character.
            std::vector<std::uint8_t> scratch(keptLines * stride);
            std::memcpy(scratch.data(), lines_.data() + srcIndex * stride, keptLines * stride);
            // Blank the entire old occupied range first -- covers both
            // the case where dst doesn't fully overlap src, and where
            // the OLD range was wider than what's kept (some of the
            // oldest lines being dropped, per keptLines < linesWritten).
            std::fill(lines_.data() + srcIndex * stride,
                     lines_.data() + static_cast<std::size_t>(oldROWS) * stride, 32);
            std::memcpy(lines_.data() + dstIndex * stride, scratch.data(), keptLines * stride);
        }
        // Blank whatever's above the moved content in the NEW layout too
        // (e.g. indices [0, dstIndex) when dstIndex > 0) -- otherwise
        // that range could still hold whatever was there before this
        // Screen was ever constructed, or from an earlier, different
        // font size's own now-unrelated content.
        if (dstIndex > 0) {
            std::fill(lines_.data(), lines_.data() + dstIndex * stride, 32);
        }
        numLines_ = keptLines;
    }

    txt_size(size);
}

void Screen::reflow(std::uint8_t oldROWS) {
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
    // FIXED (again): the repositioning below -- correct and needed for a
    // REAL vertical layout change (a font-size switch changing ROWS_) --
    // was firing unconditionally, on EVERY reflow() call, including pure
    // width-only ones (the button strip narrowing text width, e.g. when
    // the menu opens) that never touch ROWS_ at all. It used numLines_ to
    // recompute row_ -- but numLines_ was still sitting at its stale
    // pre-scroll value (see screen_pars()'s own oldROWS check, which now
    // correctly skips ITS OWN fix-up for the same width-only case) for
    // any screen that hadn't scrolled yet, so this recomputed row_ as if
    // the screen were full, yanking already-correctly-positioned text
    // (and the cursor) down to the bottom -- confirmed on real hardware:
    // opening the menu alone, font size never touched, did exactly this.
    // Only apply the repositioning when the caller actually says ROWS_
    // changed (oldROWS passed in, and different from the CURRENT ROWS_,
    // already updated by screen_pars() before this runs) -- true for
    // setTextSize()'s own call, false for setTextWidth()'s.
    if (oldROWS != 0 && oldROWS != ROWS_) {
        // Only meaningful for the "live" view (offset_ == 0) -- while
        // scrolled back into history (offset_ > 0), the cursor isn't
        // visible on screen at all regardless, so its exact row_ value
        // doesn't matter until the user scrolls back to live view anyway
        // (which resets offset_ to 0, at which point this same formula
        // applies again naturally on the next real ROWS_ change).
        if (offset_ == 0) {
            // FIXED: was always using "min(actualNumLines, ROWS_) - 1"
            // here -- correct ONLY when screen_pars()'s own pre-scroll
            // normalisation just ran (preScrollNormalized_), which
            // physically moves content to match this exact formula (see
            // its own comment). But when the screen had ALREADY scrolled
            // at least once before this switch (up() had already run),
            // that normalisation block is skipped entirely -- the
            // existing content keeps following up()'s OWN, different
            // convention instead ("index 0 = most recent", which
            // full()'s own draw loop always shows at the BOTTOM screen
            // row, i.e. ROWS_-1, regardless of how many real lines exist
            // -- unlike the pre-scroll convention, this one was never
            // tied to numLines_ at all). Using the pre-scroll formula
            // for this second case put the cursor wherever numLines_
            // happened to suggest, completely disconnected from where
            // the actual most-recent line was drawn -- confirmed on real
            // hardware as the cursor landing well away from the true end
            // of the text after switching fonts with a partially-scrolled
            // screen (enough text to have scrolled at the OLD, smaller
            // ROWS_, but not enough to fill the NEW, larger one).
            if (preScrollNormalized_) {
                const std::size_t visibleLines = actualNumLines < ROWS_ ? actualNumLines : ROWS_;
                row_ = static_cast<std::uint8_t>(visibleLines > 0 ? visibleLines - 1 : 0);
            } else {
                row_ = static_cast<std::uint8_t>(ROWS_ > 0 ? ROWS_ - 1 : 0);
            }
        } else if (row_ >= ROWS_) {
            row_ = static_cast<std::uint8_t>(ROWS_ > 0 ? ROWS_ - 1 : 0);
        }
    } else if (row_ >= ROWS_) {
        // Safety clamp regardless of the above -- shouldn't normally
        // trigger when ROWS_ itself never changed, but guards against
        // row_ ever being left out of bounds for some other reason.
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
    // Explicitly (re-)asserts both the hardware text-size register and
    // (for the normal, <=127 path just below) the foreground colour,
    // rather than relying on them still being whatever this Screen last
    // set them to. Both are single, global hardware registers, not
    // private to Screen -- on the 7" panel, the PIP menu overlay draws
    // to the SAME registers via its own txtSize()/txtColor() calls
    // (openMainMenu()'s own MenuFrame::draw(), row highlighting, etc.),
    // and can do so at any time a button is pressed, independently of
    // when Screen itself happens to draw next (new HP-IL data arriving
    // asynchronously). A one-sided fix that only restored Screen's own
    // state AFTER the menu finished drawing (tried first) doesn't cover
    // this: it doesn't know Screen might draw again before the NEXT
    // menu redraw, at which point the register could be back at the
    // menu's own value from something in between. Making Screen assert
    // its own values immediately before every character drawn removes
    // the dependency on timing/ordering with the menu entirely -- costs
    // a couple of extra register writes per character, negligible next
    // to the actual per-character SPI transfer itself, and only ever
    // matters on the 7" panel (the 5" path never has anything else
    // drawing to the shared registers in between, since it still
    // suspends Screen while a menu is open).
    d_->txtSize(size_);
    // RESTORED: was reverted to selectBuiltinFont() during earlier
    // debugging (custom/UCG font rendered nothing -- background stayed
    // blank, cursor still moved, splash bitmap still showed, but no
    // glyphs). ROOT CAUSE FOUND: LT7683's UCG (user-defined character)
    // text codes MUST be written as TWO bytes (high byte, then low
    // byte) -- even for codes that fit in one byte -- confirmed on real
    // hardware; a single-byte write (what txtWriteChar() always did
    // before this fix) leaves the text engine waiting forever for a
    // second byte that never arrives, so the character is never
    // actually committed. txtWriteChar() now dispatches to the correct
    // (2-byte) protocol automatically whenever selectCustomFont() is
    // the currently active font (see usingCustomFont_'s own comment,
    // LT7683.hpp) -- no other change needed here. Menus, which use
    // selectBuiltinFont() (1-byte codes, unaffected by this bug),
    // continuing to work throughout all of this was never actually
    // evidence against the transmission path itself -- CGROM and UCG
    // simply need different wire formats on this chip.
    d_->selectCustomFont();
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
        d_->txtColor(color_, 0);
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
    // KNOWN BUG, not yet fully fixed (this path -- size_>=4, "custom
    // CGRAM font" mode -- is currently unused; the project constructs
    // Screen with size=1 everywhere). The char/line SPACING registers
    // below are now fixed (see setCharSpacing()'s own comment,
    // LT7683.hpp) -- REG[21h]/REG[40h] below are NOT, and still need
    // the same re-derivation. Confirmed against the LT768x datasheet
    // directly:
    //   REG[21h] is MISA[15:8] -- part of MAIN IMAGE START ADDRESS, not
    //     "FNCR0/CGRAM" at all. Writing 0x80 here would move Main Image
    //     Start Address away from 0 -- likely the same "screen goes black
    //     and stays black" symptom clearActiveWindow()'s old BTE bug had,
    //     not a harmless no-op.
    //   REG[40h] ("MWCR0: text mode" here) is GCHP0 (Graphic Cursor
    //     Horizontal Position) on LT7683, not a mode register.
    // Needs the same careful re-derivation txt_size() got before size=4
    // is usable on this chip -- REG[0x40]/REG[0x21] below are STILL
    // wrong on LT7683 (this whole function remains unreachable in
    // production either way -- see this function's own opening comment).
    // The two spacing writes ARE now fixed, via the same
    // setCharSpacing()/setLineSpacing() methods txt_size() uses --
    // see their own comment (LT7683.hpp) for why raw REG[0x2E]/REG[0x29]
    // was wrong here too.
    d_->writeReg(0x40, 0x80);  // MWCR0: text mode
    d_->writeReg(0x21, 0x80);  // FNCR0: CGRAM
    d_->setCharSpacing(0);     // horizontal char spacing
    d_->setLineSpacing(0);     // vertical line spacing
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