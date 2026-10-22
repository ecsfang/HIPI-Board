// display_text.cpp -- see display_text.h (the filter and the ring; the
// firmware's store and saving are in display_text_save.cpp)
#include "display_text.h"
#include "roman8.h"
#include <algorithm>
#include <cstring>

namespace hipi {

// The firmware's one; its store is set by displayText_init()
// (display_text_save.cpp)
DisplayText& displayText() {
    static DisplayText text;
    return text;
}

void DisplayText::restart() {
    state_ = State::Text;
    roman8_ = monitor_ = afterCr_ = lastEscH_ = false;
    lastLineEmpty_ = true;
    lineLen_ = pendLen_ = 0;
    written_ = 0;
    lines_ = 0;
}

void DisplayText::put(std::uint8_t c, Model model) {
    switch (state_) {
    case State::Esc:
        state_ = State::Text;
        escape(c, model);
        return;
    case State::Pos1:                       // ESC % <col> <row>
        state_ = State::Pos2;
        return;
    case State::Pos2:
        state_ = State::Text;
        return;
    case State::Ext:                        // ESC # <c> [<arg>]
        state_ = (c != 0 && std::strchr(extArgCmds_, static_cast<char>(c)) != nullptr) ? State::ExtArg
                                                                                        : State::Text;
        return;
    case State::ExtArg:
        state_ = State::Text;
        return;
    case State::Text:
        break;
    }
    if (c == 27) {
        state_ = State::Esc;
        return;
    }
    // 92198 monitor mode: everything is shown, only ESC Z is acted on --
    // here, only the characters are kept
    if (monitor_) {
        if (c >= 32) character(c, model);
        return;
    }
    lastEscH_ = false;
    if (c == 13) {                          // CR: the line ends (an LF after it ends nothing)
        endLine();
        afterCr_ = true;
        return;
    }
    if (c == 10) {
        if (!afterCr_) endLine();
        afterCr_ = false;
        return;
    }
    afterCr_ = false;
    if (c == 8) {                           // BS: the last character goes (all its UTF-8 bytes)
        while (lineLen_ > 0 && (static_cast<std::uint8_t>(line_[lineLen_ - 1]) & 0xC0) == 0x80) --lineLen_;
        if (lineLen_ > 0) --lineLen_;
        return;
    }
    if (c < 32) return;                     // other control codes
    character(c, model);
}

void DisplayText::escape(std::uint8_t c, Model model) {
    const bool v80 = model == Model::Hp92198;
    const bool afterH = lastEscH_;
    lastEscH_ = false;
    afterCr_ = false;
    if (monitor_) {                         // (ESC Z ends it; the rest is shown as is)
        if (c == 'Z') monitor_ = false;
        return;
    }
    switch (c) {
    case '%':                               // cursor to <col> <row>: the line so far ends
        breakLine();
        state_ = State::Pos1;
        break;
    case '#': case '~':
        state_ = State::Ext;
        break;
    case 'H':                               // home
        breakLine();
        lastEscH_ = true;
        break;
    case 'A': case 'B':                     // up / down
        breakLine();
        break;
    case 'J':                               // clear to the end of the screen: after home, all of it
        if (afterH) clearedScreen();
        break;
    case 'E': case '[': case ']':           // clear (82163), soft reset (92198); size / width
        clearedScreen();
        break;
    case 'e':                               // 92198: hard reset
        if (v80) { roman8_ = false; clearedScreen(); }
        break;
    case '/': case '=':                     // 92198: NTSC / PAL (clears)
        if (v80) clearedScreen();
        break;
    case 'h':                               // 92198: home (memory)
        if (v80) breakLine();
        break;
    case 'j': if (v80) roman8_ = true;  break;
    case 'k': if (v80) roman8_ = false; break;
    case 'Y': if (v80) monitor_ = true; break;
    default:                                // the rest (cursor right/left, insert, delete ...): nothing
        break;
    }
}

void DisplayText::character(std::uint8_t c, Model model) {
    if (model == Model::Hp92198 && roman8_) {
        if (c < 128) { if (c >= 32 && c != 127) addUtf8(c); return; }
        if (c < 160) return;                // undefined
        const std::uint8_t m = kRoman8[c - 160];
        if (m == 0) addUtf8(' ');
        else if (m == 1) addUtf8(0x2588);   // the solid block
        else addUtf8(m);                    // Latin-1 = the first 256 code points
        return;
    }
    // ASCII: bit 7 is inverse video (the 92198's 128..159 are control codes)
    if (model == Model::Hp92198 && c >= 128 && c < 160) return;
    const std::uint8_t a = static_cast<std::uint8_t>(c & 0x7F);
    if (a < 32 || a == 127) return;
    addUtf8(a);
}

void DisplayText::addUtf8(std::uint32_t cp) {
    if (lineLen_ >= kLineMax) endLine();    // a very long line: broken here
    if (cp < 0x80) {
        line_[lineLen_++] = static_cast<char>(cp);
    } else if (cp < 0x800) {
        line_[lineLen_++] = static_cast<char>(0xC0 | (cp >> 6));
        line_[lineLen_++] = static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        line_[lineLen_++] = static_cast<char>(0xE0 | (cp >> 12));
        line_[lineLen_++] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        line_[lineLen_++] = static_cast<char>(0x80 | (cp & 0x3F));
    }
}

void DisplayText::endLine() {
    while (lineLen_ > 0 && line_[lineLen_ - 1] == ' ') --lineLen_;   // trailing spaces
    emit(line_, lineLen_);
    emit("\n", 1);
    lastLineEmpty_ = lineLen_ == 0;
    lineLen_ = 0;
    ++lines_;
}

// The screen was cleared: what's on it so far is a block of its own, with
// one empty line after it
void DisplayText::clearedScreen() {
    breakLine();
    if (!lastLineEmpty_) endLine();
}

void DisplayText::cleared() {
    state_ = State::Text;
    roman8_ = monitor_ = afterCr_ = lastEscH_ = false;
    clearedScreen();
}

void DisplayText::emit(const char* s, std::size_t n) {
    if (store_ == nullptr) return;
    while (n > 0) {
        const std::size_t k = std::min(n, kPendSize - pendLen_);
        std::memcpy(pend_ + pendLen_, s, k);
        pendLen_ += k;
        s += k;
        n -= k;
        if (pendLen_ == kPendSize) flushPend();
    }
}

void DisplayText::flushPend() {
    const std::uint32_t cap = store_->capacity();
    std::size_t done = 0;
    while (done < pendLen_) {
        const std::uint32_t at = static_cast<std::uint32_t>(written_ % cap);
        const std::size_t k = std::min<std::size_t>(pendLen_ - done, cap - at);
        store_->write(at, reinterpret_cast<const std::uint8_t*>(pend_ + done), k);
        written_ += k;
        done += k;
    }
    pendLen_ = 0;
}

void DisplayText::read(const std::function<void(const char*, std::size_t)>& out) const {
    if (store_ == nullptr) return;
    const std::uint32_t cap = store_->capacity();
    // The ring, oldest first: once it has run over, from the first whole line
    std::uint64_t at = written_ > cap ? written_ - cap : 0;
    bool skipping = written_ > cap;
    std::uint8_t buf[512];
    while (at < written_) {
        const std::uint32_t off = static_cast<std::uint32_t>(at % cap);
        const std::size_t k = static_cast<std::size_t>(
            std::min<std::uint64_t>({ sizeof buf, written_ - at, static_cast<std::uint64_t>(cap - off) }));
        store_->read(off, buf, k);
        at += k;
        std::size_t from = 0;
        if (skipping) {
            while (from < k && buf[from] != '\n') ++from;
            if (from == k) continue;
            ++from;
            skipping = false;
        }
        if (from < k) out(reinterpret_cast<const char*>(buf + from), k - from);
    }
    if (pendLen_ > 0) out(pend_, pendLen_);
    std::size_t n = lineLen_;
    while (n > 0 && line_[n - 1] == ' ') --n;
    if (lineLen_ > 0) {
        out(line_, n);
        out("\n", 1);
    }
}

}  // namespace hipi
