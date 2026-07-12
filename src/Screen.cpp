// SPDX-License-Identifier: MIT
//
// HP82163 video-display emulator — Screen class.
// C++17 port of the MicroPython class by J. Chilla, March 2026.

#include "Screen.hpp"

#include <algorithm>
#include <cstring>

namespace hp82163 {

// -----------------------------------------------------------------------
// Construction
// -----------------------------------------------------------------------

Screen::Screen(RA8875& display,
               std::uint16_t color,
               std::uint8_t size,
               std::uint8_t brightness,
               std::uint16_t textWidth)
    : d_(display),
      color_(color),
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
      offset_(0),
      max_(200),
      cnt_(0),
      cp_(0) {
    d_.txtColor(color, 0);
    d_.brightness(brightness);
    screen_pars(size);
    // Cursor blinking frequency (register 0x44 = CBLR)
    d_.writeReg(0x44, 0x0F);
    clear();
}

// -----------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------
void Screen::clear() {
    d_.writeReg(RA8875::MCLR,
                static_cast<std::uint8_t>(RA8875::MCLR_START | RA8875::MCLR_ACTIVE));  // <-- ACTIVE, inte FULL
    lines_.clear();
    lines_.reserve(ROWS_);
    for (std::uint8_t i = 0; i < ROWS_; ++i) {
        lines_.emplace_back(COLS_, 32);   // fill with spaces
    }
    row_ = 0;
    col_ = 0;
    cnt_ = 0;
    cp_  = 0;
    escN_ = false;
    // sleep(0.500) in the MicroPython original — keep a shorter delay here
    // since the MCLR_START is synchronous from the controller's perspective.
    d_.spiDelayMs(500);  // (declared below as a thin RA8875 helper)
    set_cur();
}

void Screen::full() {
    d_.writeReg(RA8875::MCLR,
                static_cast<std::uint8_t>(RA8875::MCLR_START | RA8875::MCLR_FULL));
    set_cursor(0, 0);
    d_.writeReg(0x40, 0x80);  // text mode, auto-incrementing, invisible cursor
    for (int row = 0; row < ROWS_; ++row) {
        const auto& line = lines_[ROWS_ - 1 - row + offset_];
        for (std::uint8_t col = 0; col < COLS_; ++col) {
            draw_letter(line[col]);
        }
    }
    set_cursor(col_, row_);
    set_cur();
}

void Screen::up(bool roll, bool cmd) {
    if (!cmd && offset_ == 0) return;

    // BTE scroll screen content up by one line.
    d_.BTE(0xC2, 0, 0,
           static_cast<std::uint16_t>(COLS_ * width()),
           static_cast<std::uint16_t>(height() * (ROWS_ - 1)),
           0, height());

    if (roll) {
        set_cursor(0, static_cast<std::uint8_t>(ROWS_ - 1));
        d_.writeReg(0x40, 0x80);  // text mode, auto-incrementing, invisible cursor
        for (std::uint8_t col = 0; col < COLS_; ++col) {
            draw_letter(lines_[offset_ > 0 ? offset_ - 1 : 0][col]);
        }
        set_cursor(col_, row_);
        set_cur();
        if (cmd) {
            auto last = std::move(lines_.back());
            lines_.pop_back();
            lines_.insert(lines_.begin(), std::move(last));
        } else {
            offset_ = (offset_ > 0) ? offset_ - 1 : 0;
        }
    } else {
        if (cmd) {
            d_.BTE(0x06, 0, static_cast<std::uint16_t>(height() * (ROWS_ - 1)),
                   static_cast<std::uint16_t>(width() * COLS_),
                   height());   // blank last line
            lines_.insert(lines_.begin(), std::vector<std::uint8_t>(COLS_, 32));
            if (lines_.size() > max_) lines_.pop_back();
        } else {
            set_cursor(0, static_cast<std::uint8_t>(ROWS_ - 1));
            d_.writeReg(0x40, 0x80);
            for (std::uint8_t col = 0; col < COLS_; ++col) {
                draw_letter(lines_[offset_ - 1][col]);
            }
            set_cursor(col_, row_);
            set_cur();
            offset_ = (offset_ > 0) ? offset_ - 1 : 0;
        }
    }
}

void Screen::down(bool cmd) {
    if (!cmd && offset_ >= lines_.size() - ROWS_) return;

    for (int row = ROWS_; row > 0; --row) {
        d_.BTE(0xC2, 0, static_cast<std::uint16_t>(row * height()),
               static_cast<std::uint16_t>(COLS_ * width()),
               height(),
               0, static_cast<std::uint16_t>((row - 1) * height()));
    }
    set_cursor(0, 0);
    d_.writeReg(0x40, 0x80);
    for (std::uint8_t col = 0; col < COLS_; ++col) {
        draw_letter(lines_[offset_ + ROWS_][col]);
    }
    set_cursor(col_, row_);
    set_cur();

    if (cmd) {
        auto first = std::move(lines_.front());
        lines_.erase(lines_.begin());
        lines_.push_back(std::move(first));
    } else {
        const std::size_t cap = lines_.size() > ROWS_ ? lines_.size() - ROWS_ : 0;
        offset_ = (offset_ + 1 <= cap) ? offset_ + 1 : cap;
    }
}

void Screen::store() {
    d_.BTE(0xC2, 0, 0x8000, 800, 480);
}

void Screen::recall() {
    d_.BTE(0xC2, 0, 0, 800, 480, 0, 0x8000);
}

void Screen::inschar() {
    auto& line = lines_[ROWS_ - 1 - row_];
    std::uint8_t last = line.back();
    line.pop_back();

    d_.BTE(0xC2, 0, 0x8000, 800, static_cast<std::uint16_t>(height()), 0,
           static_cast<std::uint16_t>(row_ * height()));   // store line

    if (col_ != COLS_ - 1) {
        d_.BTE(0xC2,
               (col() + 1) * width(),
               row() * height(),
               800 - (col() + 1) * width(),
               height(),
               col() * width(),
               0x8000);                                    // restore shifted line
        for (std::size_t i = COLS_ - 1 - row_; i < lines_.size(); ++i) {
            // shift within logical buffer
        }
        // Python: lines[ROWS-1-row][col+1:] = lines[ROWS-1-row][col:]
        // (operates on a different copy of the row buffer; equivalent to)
        auto& L = lines_[ROWS_ - 1 - row_];
        L.insert(L.begin() + col_ + 1, L.begin() + col_, L.end() - 1);
    } else {
        line.push_back(last);
    }

    if (!((cnt_ < COLS_) || (cp_ > COLS_))) {
        if ((cnt_ == COLS_) && (row_ == ROWS_ - 1)) {
            up();
            row_ = static_cast<std::uint8_t>(row_ - 1);
        }
        // insert first char of next line at end of current line
        auto& prev = lines_[ROWS_ - 2 - row_];
        prev.insert(prev.begin(), last);
        std::uint8_t prev_last = prev.back();
        prev.pop_back();
        d_.BTE(0xC2, 0, 0x8000, width(), height(),
               static_cast<std::uint16_t>(800 - width()), 0x8000);
        d_.BTE(0xC2, width(), 0x8000,
               static_cast<std::uint16_t>(800 - width()),
               height(), 0,
               static_cast<std::uint16_t>((row_ + 1) * height()));
        d_.BTE(0xC2, 0, static_cast<std::uint16_t>((row_ + 1) * height()),
               800, height(), 0, 0x8000);
        last = prev_last;
    }
    cnt_ = static_cast<std::uint8_t>(cnt_ + 1);
}

void Screen::txt_size(std::uint8_t size) {
    if (size < 4) {
        d_.txtSize(size);
        d_.writeReg(0x2E, 0);  // horizontal char spacing
        d_.writeReg(0x29, 0);  // vertical line spacing
    } else {
        d_.txtSize(0);
        fon_mode();
    }
}

void Screen::screen_pars(std::uint8_t size) {
    size_ = size;
    if (size < 4) {
        const std::uint8_t k = static_cast<std::uint8_t>(1 + size);
        width_  = static_cast<std::uint16_t>(8 * k);
        COLS_   = static_cast<std::uint8_t>(textWidth_ / width_);   // <-- var: 100 / k
        height_ = static_cast<std::uint16_t>(16 * k);
        ROWS_   = static_cast<std::uint8_t>(30 / k);
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
    txt_size(size);
}

void Screen::cursor(std::uint8_t cur) {
    switch (cur) {
        case 65:  // up
            if (row_ != 0) row_ = static_cast<std::uint8_t>(row_ - 1);
            break;
        case 66:  // down
            if (row_ != ROWS_ - 1) row_ = static_cast<std::uint8_t>(row_ + 1);
            break;
        case 67:  // right
            cp_ = static_cast<std::uint8_t>(cp_ + 1);
            if (col_ < COLS_ - 1) {
                col_ = static_cast<std::uint8_t>(col_ + 1);
            } else {
                col_ = 0;
                row_ = static_cast<std::uint8_t>(row_ + 1);
                if (row_ == ROWS_) row_ = 0;
            }
            break;
        case 68:  // left
            cp_ = (cp_ > 0) ? static_cast<std::uint8_t>(cp_ - 1) : 0;
            if (col_ > 0) {
                col_ = static_cast<std::uint8_t>(col_ - 1);
            } else if (row_ > 0) {
                row_ = static_cast<std::uint8_t>(row_ - 1);
                col_ = static_cast<std::uint8_t>(COLS_ - 1);
            }
            break;
        case 72:  // home
            col_ = 0;
            row_ = 0;
            break;
        default:
            // cursor-mode commands
            switch (cur) {
                case 60: cv_ = false; break;
                case 62: cv_ = true;  break;
                case 81:
                    Ins_ = true;
                    break;
                case 82:
                    Ins_ = false;
                    escN_ = false;
                    break;
                default: return;
            }
            set_cur();
            return;
    }
    set_cursor(col_, row_);
}

void Screen::cursor_pos(std::uint8_t row, std::uint8_t col) {
    // Original Python:
    //   self.col = pos[0] % self.COLS
    //   self.row = pos[1] % self.ROWS
    // (where pos[0]=column, pos[1]=row from the HP-41 stream)
    col_ = static_cast<std::uint8_t>(col % COLS_);
    row_ = static_cast<std::uint8_t>(row % ROWS_);
    set_cursor(col_, row_);
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
            cursor(37);  // -> set_cursor via the 37 case
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
            for (std::uint8_t cc = col_; cc < COLS_; ++cc) {
                lines_[ROWS_ - 1 - row_][cc] = 32;
            }
            d_.BTE(0x06,
                   col_ * width(),
                   row_ * height(),
                   width() * (COLS_ - col_),
                   height());
            if (c == 74) {
                for (std::uint8_t i = 0; i < ROWS_ - 1 - row_; ++i) {
                    lines_[i].assign(COLS_, 32);
                }
                if (row_ != ROWS_ - 1) {
                    d_.BTE(0x06, 0,
                           height() * (row_ + 1),
                           width() * COLS_,
                           height() * (ROWS_ - row_ - 1));
                }
            }
        } else if (c == 76) {            // ESC L -> insert line
            if (row_ != ROWS_ - 1) {
                d_.BTE(0xC2, 0, 0x8000, 800,
                       (ROWS_ - 1 - row_) * height(),
                       0, height() * row_);
                d_.BTE(0xC2, 0,
                       height() * (row_ + 1), 800,
                       (ROWS_ - 1 - row_) * height(),
                       0, 0x8000);
            }
            d_.BTE(0x06, 0, height() * row(),
                   width() * cols(), height());
            lines_.insert(lines_.begin() + (ROWS_ - 1 - row_),
                          std::vector<std::uint8_t>(COLS_, 32));
            if (lines_.size() > 0) lines_.erase(lines_.begin());
            col_ = 0;
            set_cursor(col_, row_);
        } else if (c == 77) {            // ESC M -> delete line
            if (row_ != ROWS_ - 1) {
                d_.BTE(0xC2, 0, row_ * height(),
                       COLS_ * width(),
                       height() * (ROWS_ - 1 - row_),
                       0, height() * (row_ + 1));
            }
            d_.BTE(0x06, 0, height() * (ROWS_ - 1),
                   width() * COLS_, height());
            lines_.erase(lines_.begin() + (ROWS_ - 1 - row_));
            lines_.insert(lines_.begin(), std::vector<std::uint8_t>(COLS_, 32));
        } else if (c == 78) {            // ESC N -> enter insert mode
            escN_ = true;
        } else if (c == 79) {            // ESC O -> delete character
            {
                auto& line = lines_[ROWS_ - 1 - row_];
                if ((uint8_t)(col_ + 1) < line.size()) {
                    line.erase(line.begin() + col_, line.begin() + col_ + 1);
                }
            }
            if (col_ != COLS_ - 1) {
                d_.BTE(0xC2,
                       col_ * width(),
                       row_ * height(),
                       width() * (COLS_ - col_ - 1),
                       height(),
                       (col_ + 1) * width(),
                       row_ * height());
            }
            if ((cnt_ < COLS_) || (cp_ > COLS_)) {
                lines_[ROWS_ - 1 - row_].push_back(32);
                d_.BTE(0x06,
                       (COLS_ - 1) * width(),
                       row_ * height(),
                       width(), height());
            } else {
                lines_[ROWS_ - 1 - row_].push_back(lines_[ROWS_ - 2 - row_][0]);
                d_.BTE(0xC2,
                       (COLS_ - 1) * width(),
                       row_ * height(),
                       width(), height(),
                       0, (row_ + 1) * height());
                {
                    auto& prev = lines_[ROWS_ - 2 - row_];
                    prev.erase(prev.begin());
                }
                d_.BTE(0xC2, 0, (row_ + 1) * height(),
                       width() * (COLS_ - 1),
                       height(),
                       width(), (row_ + 1) * height());
                lines_[ROWS_ - 2 - row_].push_back(32);
                d_.BTE(0x06,
                       (COLS_ - 1) * width(),
                       (row_ + 1) * height(),
                       width(), height());
            }
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
                set_cursor(col_, row_);
            }
        } else {
            // printable character
            nline_ = false;
            if (escN_)
                inschar();
            lines_[ROWS_ - 1 - row_][col_] = c;
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
            set_cursor(col_, row_);
        }
    }
}

// -----------------------------------------------------------------------
// Internals
// -----------------------------------------------------------------------

void Screen::set_cursor(std::uint8_t c, std::uint8_t r) {
    d_.writeReg16(0x2A, static_cast<std::uint16_t>(c * width() + ofx_));
    d_.writeReg16(0x2C, static_cast<std::uint16_t>(r * height() + ofy_));
}

void Screen::set_cur() {
    if (cv_) {
        // text mode + visible blinking cursor + auto-increment disabled
        d_.writeReg(0x40, 0xE2);
        if (Ins_) d_.writeReg(0x4F, 0x00);     // underscore cursor
        else      d_.writeReg(0x4F, 0x0F);     // full-height cursor
    } else {
        d_.writeReg(0x40, 0x82);                // invisible cursor
    }
    set_cursor(col_, row_);
}

void Screen::draw_letter(std::uint8_t c) {
    if (c > 127) {
        if (size_ < 4) {
            d_.txtColor(0, color_);
            d_.txtWriteChar(static_cast<std::uint8_t>(c & 0x7F));
        } else {
            d_.fillRect(static_cast<std::int16_t>(col_ * width()),
                        static_cast<std::int16_t>(row_ * height()),
                        static_cast<std::int16_t>(width()),
                        static_cast<std::int16_t>(height()),
                        color_);
            d_.txtColor(0, color_);
            const char tmp[2] = { static_cast<char>(c & 0x7F), 0 };
            fon_write(tmp);
        }
        d_.txtColor(color_, 0);
    } else {
        if (size_ < 4) {
            d_.txtWriteChar(c);
        } else {
            const char tmp[2] = { static_cast<char>(c), 0 };
            fon_write(tmp);
        }
    }
}

void Screen::fon_mode() {
//    if (d_.mode() != nullptr && std::strcmp(d_.mode(), "fon") == 0) return;
    d_.writeReg(0x40, 0x80);  // MWCR0: text mode
    d_.writeReg(0x21, 0x80);  // FNCR0: CGRAM
    d_.writeReg(0x2E, 2);     // horizontal char spacing
    d_.writeReg(0x29, 4);     // vertical line spacing
    // We don't expose "fon" via RA8875::mode(); tag it with a no-op write.
    // (See note in Screen.hpp — we track mode implicitly via size_.)
}

void Screen::fon_write(const char* s) {
    fon_mode();
    d_.writeCmd(RA8875::MRWC);
    for (const char* p = s; *p; ++p) {
        d_.writeData(static_cast<std::uint8_t>(*p));
        if (d_.txtScale() > 0) d_.spiDelayMs(1);
    }
}

// Special-case cursor command 37 ("ESC %" with row, col already in pos_)
namespace {
}  // namespace
}  // namespace hp82163
