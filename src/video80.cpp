// video80.cpp -- see video80.h
#include "video80.h"
#include "roman8.h"
#include <algorithm>
#include <cstring>

namespace hipi {

namespace {

// The fonts (see video80.h): the chip's 12x24 (7" only) or 8x16
constexpr bool kHasLargeFont = true;
constexpr std::uint16_t kControlColor = 0x07FF;   // control codes shown in monitor mode

}  // namespace

Video80::Video80(DisplayDriver* d) : d_(d) {
    setSmallFont(false);
    hardReset();
}

void Video80::setSmallFont(bool small) {
    small = small || !kHasLargeFont;
    if (small == small_ && glyphW_ != 0) return;
    small_ = small;
    fontCode_ = small ? 0 : 1;              // CCR0 bits 5:4: 8x16 / 12x24
    glyphW_ = small ? 8 : 12;
    glyphH_ = small ? 16 : 24;
    geometry();
    needAll_ = true;
    drawAll();
}

void Video80::setDefaultForty(bool forty) {
    if (forty == defaultForty_) return;
    defaultForty_ = forty;
    setWidth(forty);                        // now too (clears, like ESC [ / ])
}

void Video80::geometry() {
    const int hx = cols_ == 40 ? 2 : 1;
    cw_ = glyphW_ * hx;
    ch_ = glyphH_;
    ox_ = (SCREEN_MAX_X - cols_ * cw_) / 2;
    oy_ = (SCREEN_MAX_Y - rows_ * ch_) / 2;
}

// ═══ Model ════════════════════════════════════════════════════════════

void Video80::clearAll() {
    std::memset(buf_, ' ', sizeof(buf_));
    std::memset(cont_, 0, sizeof(cont_));
    top_ = 0;
    cr_ = cc_ = 0;
    wrapPending_ = false;
    live_ = true;
}

void Video80::hardReset() {
    cols_ = defaultForty_ ? 40 : 80;
    rows_ = defaultForty_ ? 20 : 24;
    cursorOn_ = true;
    insert_ = insertCursor_ = false;
    monitor_ = roman8_ = pal_ = false;
    esc_ = Esc::None;
    geometry();
    clearAll();
    needAll_ = true;
    drawAll();
}

void Video80::setWidth(bool forty) {
    cols_ = forty ? 40 : 80;
    rows_ = forty ? 20 : 24;
    geometry();
    clearAll();
    needAll_ = true;
    drawAll();
}

std::uint8_t Video80::status() const {
    std::uint8_t s = 0;
    if (cols_ == 40) s |= 0x01;
    if (roman8_)     s |= 0x02;
    if (pal_)        s |= 0x04;
    if (!cursorOn_)  s |= 0x08;
    if (insert_)     s |= 0x10;
    return s;
}

void Video80::lineText(int line, char* out) const {
    for (int c = 0; c < cols_; ++c) {
        const std::uint8_t b = buf_[line][c];
        out[c] = (b >= 32 && b < 127) ? static_cast<char>(b) : (b == ' ' ? ' ' : '.');
    }
    out[cols_] = 0;
}

void Video80::put(std::uint8_t c) {
    // Monitor mode: everything is shown, only ESC Z is acted on
    if (monitor_) {
        if (esc_ == Esc::MonEsc) {
            esc_ = Esc::None;
            if (c == 'Z') { monitor_ = false; return; }
        }
        if (c == 27) esc_ = Esc::MonEsc;
        printable(c == 0 ? ' ' : c);
        placeCursor();
        return;
    }
    switch (esc_) {
        case Esc::Esc:
            esc_ = Esc::None;
            escape(c);
            placeCursor();
            return;
        case Esc::PosCol:
            posCol_ = c;
            esc_ = Esc::PosRow;
            return;
        case Esc::PosRow:
            // ESC % c r: the screen position (beyond the screen: its edge)
            esc_ = Esc::None;
            cc_ = std::min<int>(posCol_, cols_ - 1);
            cr_ = std::min<int>(c, rows_ - 1);
            wrapPending_ = false;
            placeCursor();
            return;
        case Esc::Ext:
            esc_ = Esc::None;
            if (c != 0 && std::strchr(extArgCmds_, static_cast<char>(c)) != nullptr) {
                extCmd_ = c;
                esc_ = Esc::ExtArg;
            } else if (extCommand_) {
                extCommand_(c, 0);
            }
            return;
        case Esc::ExtArg:
            esc_ = Esc::None;
            if (extCommand_) extCommand_(extCmd_, c);
            return;
        default:
            break;
    }
    if (c == 27) { esc_ = Esc::Esc; return; }
    if (c < 32) { control(c); placeCursor(); return; }
    // 128..159: control codes too (inverse ones in ASCII, undefined in
    // Roman-8) -- only shown in monitor mode
    if (c >= 128 && c < 160) return;
    printable(c);
    placeCursor();
}

void Video80::control(std::uint8_t c) {
    switch (c) {
        case 13:                                   // CR
            cc_ = 0;
            wrapPending_ = false;
            break;
        case 10:                                   // LF
            wrapPending_ = false;
            lineFeed();
            break;
        case 8:                                    // BS: back, to the end of the line above
            wrapPending_ = false;
            if (cc_ > 0) --cc_;
            else if (cr_ > 0) { --cr_; cc_ = cols_ - 1; }
            break;
        default:                                   // the others: ignored
            break;
    }
}

// LF: down a line; on the window's last line the screen scrolls -- the
// window moves down through the memory (clearing the line coming into
// view), or at the end of the memory everything moves up (the first
// line is lost)
void Video80::lineFeed() {
    if (cr_ < rows_ - 1) { ++cr_; return; }
    if (top_ + rows_ < kBufLines) {
        ++top_;
        clearLine(top_ + rows_ - 1);
        cont_[top_ + rows_ - 1] = false;
    } else {
        std::memmove(buf_[0], buf_[1], sizeof(buf_[0]) * (kBufLines - 1));
        std::memmove(cont_, cont_ + 1, sizeof(cont_[0]) * (kBufLines - 1));
        clearLine(kBufLines - 1);
        cont_[kBufLines - 1] = false;
        cont_[0] = false;
    }
    scrolledUp();
}

void Video80::clearLine(int line, int from) {
    if (from < kMaxCols) std::memset(&buf_[line][from], ' ', static_cast<std::size_t>(kMaxCols - from));
}

void Video80::printable(std::uint8_t c) {
    if (wrapPending_) {
        // Past the end of the line: it goes on on the next (implicit CR LF)
        wrapPending_ = false;
        cc_ = 0;
        lineFeed();
        cont_[line()] = true;
    }
    if (insert_) {
        insertCharAt(line(), cc_, c);
    } else {
        buf_[line()][cc_] = c;
        drawCell(line(), cc_);
    }
    if (cc_ < cols_ - 1) ++cc_;
    else wrapPending_ = true;
}

// Inserts c at line/col; what's pushed out of the line goes on at the
// start of the next one if that continues this line, else into a new
// line (only if it isn't a space)
void Video80::insertCharAt(int ln, int col, std::uint8_t c, int depth) {
    const std::uint8_t out = buf_[ln][cols_ - 1];
    std::memmove(&buf_[ln][col + 1], &buf_[ln][col], static_cast<std::size_t>(cols_ - 1 - col));
    buf_[ln][col] = c;
    int last = ln;
    if (ln + 1 < kBufLines && depth < kBufLines) {
        if (cont_[ln + 1]) {
            insertCharAt(ln + 1, 0, out, depth + 1);
            last = kBufLines - 1;
        } else if (out != ' ') {
            insertLineAt(ln + 1);
            cont_[ln + 1] = true;
            buf_[ln + 1][0] = out;
            last = kBufLines - 1;
        }
    }
    if (depth == 0) drawLines(ln, last);
}

// Deletes the character at line/col; the line's continuation (if any)
// moves back one character
void Video80::deleteCharAt(int ln, int col, int depth) {
    std::memmove(&buf_[ln][col], &buf_[ln][col + 1], static_cast<std::size_t>(cols_ - 1 - col));
    int last = ln;
    if (ln + 1 < kBufLines && cont_[ln + 1] && depth < kBufLines) {
        buf_[ln][cols_ - 1] = buf_[ln + 1][0];
        deleteCharAt(ln + 1, 0, depth + 1);
        last = kBufLines - 1;
    } else {
        buf_[ln][cols_ - 1] = ' ';
    }
    if (depth == 0) drawLines(ln, last);
}

void Video80::insertLineAt(int ln) {
    if (ln >= kBufLines) return;
    std::memmove(buf_[ln + 1], buf_[ln], sizeof(buf_[0]) * static_cast<std::size_t>(kBufLines - 1 - ln));
    std::memmove(cont_ + ln + 1, cont_ + ln, sizeof(cont_[0]) * static_cast<std::size_t>(kBufLines - 1 - ln));
    clearLine(ln);
    cont_[ln] = false;
}

void Video80::deleteLineAt(int ln) {
    const bool wasCont = cont_[ln];
    std::memmove(buf_[ln], buf_[ln + 1], sizeof(buf_[0]) * static_cast<std::size_t>(kBufLines - 1 - ln));
    std::memmove(cont_ + ln, cont_ + ln + 1, sizeof(cont_[0]) * static_cast<std::size_t>(kBufLines - 1 - ln));
    clearLine(kBufLines - 1);
    cont_[kBufLines - 1] = false;
    // The first line of a long line gone: its continuation stands alone
    if (!wasCont) cont_[ln] = false;
}

void Video80::escape(std::uint8_t c) {
    wrapPending_ = false;
    switch (c) {
        case '>': cursorOn_ = true;  break;
        case '<': cursorOn_ = false; break;
        case 'A': if (cr_ > 0) --cr_; break;                       // up, not past the top
        case 'B': if (cr_ < rows_ - 1) ++cr_; break;               // down, not past the bottom
        case 'C':                                                  // right, wrapping
            if (++cc_ >= cols_) { cc_ = 0; if (++cr_ >= rows_) cr_ = 0; }
            break;
        case 'D':                                                  // left, wrapping back, not past 0,0
            if (cc_ > 0) --cc_;
            else if (cr_ > 0) { --cr_; cc_ = cols_ - 1; }
            break;
        case 'E':                                                  // soft reset: memory cleared
            clearAll();
            needAll_ = true;
            drawAll();
            break;
        case 'H': cr_ = 0; cc_ = 0; break;                         // home (screen)
        case 'J':                                                  // clear to the end of the screen
            clearLine(line(), cc_);
            for (int r = cr_ + 1; r < rows_; ++r) { clearLine(top_ + r); cont_[top_ + r] = false; }
            drawLines(line(), top_ + rows_ - 1);
            break;
        case 'K':                                                  // clear to the end of the line
            clearLine(line(), cc_);
            drawLines(line(), line());
            break;
        case 'L':                                                  // insert line
            insertLineAt(line());
            if (line() + 1 < kBufLines) cont_[line() + 1] = false;   // (a long line is cut here)
            cc_ = 0;
            drawLines(line(), kBufLines - 1);
            break;
        case 'M':                                                  // delete line
            deleteLineAt(line());
            drawLines(line(), kBufLines - 1);
            break;
        case 'N': insert_ = true; insertCursor_ = true; break;     // insert mode
        case 'O': deleteCharAt(line(), cc_); break;                // delete character
        case 'Q': insertCursor_ = true; break;                     // insert cursor only
        case 'R': insert_ = false; insertCursor_ = false; break;   // replace mode
        case 'S':                                                  // window down (text up)
            if (top_ + rows_ < kBufLines) { ++top_; scrolledUp(); }
            break;
        case 'T':                                                  // window up (text down)
            if (top_ > 0) { --top_; needAll_ = true; drawAll(); }
            break;
        case 'Y': monitor_ = true; break;
        case 'Z': monitor_ = false; break;
        case '[': setWidth(false); break;                          // 80 x 24 (clears)
        case ']': setWidth(true); break;                           // 40 x 20 (clears)
        case '/': pal_ = false; clearAll(); needAll_ = true; drawAll(); break;   // NTSC
        case '=': pal_ = true;  clearAll(); needAll_ = true; drawAll(); break;   // PAL
        case 'e': hardReset(); break;
        case 'h': top_ = 0; cr_ = 0; cc_ = 0; needAll_ = true; drawAll(); break;  // home (memory)
        case 'j': roman8_ = true;  needAll_ = true; drawAll(); break;
        case 'k': roman8_ = false; needAll_ = true; drawAll(); break;
        case 'z': break;                                           // RAM self-test: always good
        case '%': esc_ = Esc::PosCol; break;
        case '#': case '~': esc_ = Esc::Ext; break;                // HIPI's own (see video80.h)
        default: break;                                            // not recognised: ignored
    }
}

// ═══ Drawing ══════════════════════════════════════════════════════════

bool Video80::canDraw() const {
    if (!active_ || (mayDraw_ && !mayDraw_())) return false;
    // Only on the panel itself -- not while something (the menu) has
    // pointed the drawing at another layer
    if (d_->canvasAddr() != 0) return false;
    return true;
}

void Video80::setActive(bool on) {
    if (on == active_) return;
    active_ = on;
    if (on) {
        needAll_ = true;
        drawAll();
    } else {
        d_->setTextCursorShape(false, glyphW_, glyphH_);
        // Leaves the text engine as the other views expect it
        d_->selectBuiltinFont();
        d_->txtScaleXY(1, 1);
    }
}

void Video80::setColor(std::uint16_t fg) {
    if (fg == fg_) return;
    fg_ = fg;
    needAll_ = true;
    drawAll();                              // (now, or when it may)
}

void Video80::scrollView(int n) {
    const int maxTop = kBufLines - rows_;
    const int from = viewTop();
    const int to = std::max(0, std::min(maxTop, from - n));
    if (to == from && !live_) return;
    viewTop_ = to;
    live_ = (to == top_);
    needAll_ = true;
    drawAll();
}

void Video80::viewLive() {
    if (live_) return;
    live_ = true;
    needAll_ = true;
    drawAll();
}

void Video80::prepareText() {
    d_->setActiveWindow(0, 0, SCREEN_MAX_X - 1, SCREEN_MAX_Y - 1);
    d_->selectBuiltinFontSize(fontCode_);
    d_->txtScaleXY(static_cast<std::uint8_t>(cols_ == 40 ? 2 : 1), 1);
}

// What a memory byte shows as: the font code, inverse video, and whether
// it's a control code (monitor mode)
void Video80::glyph(std::uint8_t b, std::uint8_t& code, bool& inverse, bool& special) const {
    inverse = false;
    special = false;
    if (roman8_) {
        if (b < 32) { code = static_cast<std::uint8_t>(b + 64); special = true; return; }
        if (b < 128) { code = b; return; }
        if (b < 160) { code = ' '; return; }
        const std::uint8_t m = kRoman8[b - 160];
        if (m == 1) { code = ' '; inverse = true; return; }      // the solid block
        code = m == 0 ? ' ' : m;
        return;
    }
    inverse = b >= 128;
    const std::uint8_t a = b & 0x7F;
    if (a < 32) { code = static_cast<std::uint8_t>(a + 64); special = true; return; }
    code = a;
}

void Video80::drawRow(int screenRow) {
    const int ln = viewTop() + screenRow;
    d_->txtSetCursor(static_cast<std::uint16_t>(ox_), static_cast<std::uint16_t>(oy_ + screenRow * ch_));
    std::uint16_t curFg = 0, curBg = 0;
    bool first = true;
    for (int c = 0; c < cols_; ++c) {
        std::uint8_t code;
        bool inv, special;
        glyph(buf_[ln][c], code, inv, special);
        const std::uint16_t f0 = special ? kControlColor : fg_;
        const std::uint16_t f = inv ? bg_ : f0, b = inv ? f0 : bg_;
        if (first || f != curFg || b != curBg) {
            d_->txtColor(f, b);
            curFg = f; curBg = b; first = false;
        }
        d_->txtWriteChar(code);
    }
}

// One memory cell changed: drawn if in view
void Video80::drawCell(int ln, int col) {
    if (!canDraw()) { needAll_ = true; return; }
    if (needAll_) { drawAll(); return; }
    const int r = ln - viewTop();
    if (r < 0 || r >= rows_) return;
    prepareText();
    std::uint8_t code;
    bool inv, special;
    glyph(buf_[ln][col], code, inv, special);
    const std::uint16_t f0 = special ? kControlColor : fg_;
    d_->txtColor(inv ? bg_ : f0, inv ? f0 : bg_);
    d_->txtSetCursor(static_cast<std::uint16_t>(ox_ + col * cw_), static_cast<std::uint16_t>(oy_ + r * ch_));
    d_->txtWriteChar(code);
}

// Memory lines fromLine..toLine changed: draws those in view
void Video80::drawLines(int fromLine, int toLine) {
    if (!canDraw()) { needAll_ = true; return; }
    if (needAll_) { drawAll(); return; }
    const int vt = viewTop();
    const int r0 = std::max(fromLine - vt, 0), r1 = std::min(toLine - vt, rows_ - 1);
    if (r1 < r0) return;
    d_->setTextCursorShape(false, glyphW_, glyphH_);   // no cursor jumping about
    prepareText();
    for (int r = r0; r <= r1; ++r) drawRow(r);
    placeCursor();
}

void Video80::drawAll() {
    if (!canDraw()) { if (active_) needAll_ = true; return; }
    needAll_ = false;
    d_->setTextCursorShape(false, glyphW_, glyphH_);
    d_->setActiveWindow(0, 0, SCREEN_MAX_X - 1, SCREEN_MAX_Y - 1);
    d_->fillRect(0, 0, SCREEN_MAX_X, SCREEN_MAX_Y, bg_);
    prepareText();
    for (int r = 0; r < rows_; ++r) drawRow(r);
    placeCursor();
}

void Video80::drawRegion(int x, int y, int w, int h) {
    if (!canDraw()) { needAll_ = true; return; }
    if (needAll_) { drawAll(); return; }
    d_->setActiveWindow(0, 0, SCREEN_MAX_X - 1, SCREEN_MAX_Y - 1);
    d_->fillRect(static_cast<std::int16_t>(x), static_cast<std::int16_t>(y),
                 static_cast<std::int16_t>(w), static_cast<std::int16_t>(h), bg_);
    const int r0 = std::max(0, (y - oy_) / ch_), r1 = std::min(rows_ - 1, (y + h - 1 - oy_) / ch_);
    if (r1 >= r0 && y + h > oy_ && y < oy_ + rows_ * ch_) {
        d_->setTextCursorShape(false, glyphW_, glyphH_);
        prepareText();
        for (int r = r0; r <= r1; ++r) drawRow(r);
    }
    placeCursor();
}

// The window's content moved up a line (LF at the bottom, ESC S)
void Video80::scrolledUp() {
    if (!live_) {
        // Looking back: the view stays where it is, unless the memory
        // itself moved under it
        needAll_ = true;
        drawAll();
        return;
    }
    if (!canDraw()) { needAll_ = true; return; }
    if (needAll_) { drawAll(); return; }
    // Move the picture up a text line in the chip (two BTE copies), then
    // draw only the new bottom line
    d_->setTextCursorShape(false, glyphW_, glyphH_);
    d_->bteScrollShift(static_cast<std::int16_t>(ox_), static_cast<std::int16_t>(oy_),
                       static_cast<std::uint16_t>(cols_ * cw_), static_cast<std::uint16_t>(rows_ * ch_),
                       static_cast<std::uint16_t>(ch_));
    prepareText();
    drawRow(rows_ - 1);
    placeCursor();
}

void Video80::catchUp() {
    if (needAll_) { drawAll(); return; }    // (places the cursor too)
    if (!canDraw()) return;
    prepareText();                          // font and scale: the cursor's size rides on them
    placeCursor();
}

void Video80::placeCursor() {
    if (!canDraw()) return;
    const bool show = cursorOn_ && live_ && (!cursorMayShow_ || cursorMayShow_());
    d_->txtSetCursor(static_cast<std::uint16_t>(ox_ + cc_ * cw_), static_cast<std::uint16_t>(oy_ + cr_ * ch_));
    d_->txtColor(fg_, bg_);                 // (the cursor has the text colour)
    // Block in replace mode, an underline for the insert cursor; sizes in
    // base pixels (the 40-column doubling applies by itself)
    d_->setTextCursorShape(show, glyphW_, static_cast<std::uint8_t>(insertCursor_ ? 2 : glyphH_));
}

}  // namespace hipi
