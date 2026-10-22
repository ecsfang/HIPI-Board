// display_text_test.cpp -- the display's text log (src/display_text.cpp):
// the filter for both models, the ring running over, the SDRAM store
// (writeSdram / readSdram against the emulated chip) and the bytes coming
// in over HP-IL through the display device (src/display.cpp).
//
//   make display_text_test && ./display_text_test   (in tools/hipiview)
#include "LT7683.hpp"
#include "Screen.hpp"
#include "display.h"
#include "display_text.h"
#include "video80.h"
#include "../lt7683_emu.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

std::uint64_t fakeNowUs = 1000000;
bool bTrace = false, bExtTrace = false, usb_connected = true;
hipi::Screen* screen = nullptr;      // (display.cpp's, for the HP 82163 model)

namespace {

using Model = hipi::DisplayText::Model;

LT7683Emu chip;

class ChipTransport : public hipi::DisplayTransport {
public:
    void csLow() override { first_ = true; }
    void csHigh() override {}
    void rstLow() override {}
    void rstHigh() override {}
    void spiTransfer(const std::uint8_t* tx, std::uint8_t* rx, std::size_t len) override {
        for (std::size_t i = 0; i < len; ++i) {
            const std::uint8_t b = tx ? tx[i] : 0;
            if (first_) { type_ = b & 0xC0; first_ = false; if (rx) rx[i] = 0; continue; }
            std::uint8_t r = 0;
            switch (type_) {
            case 0x00: chip.hostCmd(b); break;
            case 0x80: chip.hostWrite(b); break;
            case 0xC0: r = chip.hostRead(); break;
            default:   r = chip.hostStatus(); break;
            }
            if (rx) rx[i] = r;
        }
    }
    void delayMs(std::uint32_t) override {}
    void delayUs(std::uint32_t) override {}
private:
    bool first_ = true;
    std::uint8_t type_ = 0;
};

// The text in plain RAM
class VectorStore : public hipi::DisplayTextStore {
public:
    explicit VectorStore(std::size_t n) : mem_(n, 0xEE) {}
    std::uint32_t capacity() const override { return static_cast<std::uint32_t>(mem_.size()); }
    void write(std::uint32_t off, const std::uint8_t* d, std::size_t n) override {
        if (off + n > mem_.size()) { bad = true; return; }
        std::memcpy(mem_.data() + off, d, n);
    }
    void read(std::uint32_t off, std::uint8_t* d, std::size_t n) override {
        if (off + n > mem_.size()) { bad = true; return; }
        std::memcpy(d, mem_.data() + off, n);
    }
    bool bad = false;
private:
    std::vector<std::uint8_t> mem_;
};

// ... and in the chip's SDRAM, as in the firmware
class ChipStore : public hipi::DisplayTextStore {
public:
    explicit ChipStore(hipi::LT7683& d) : d_(d) {}
    std::uint32_t capacity() const override { return hipi::LT7683::kTextLogBytes; }
    void write(std::uint32_t off, const std::uint8_t* data, std::size_t n) override {
        d_.writeSdram(hipi::LT7683::kTextLogAddr + off, data, n);
    }
    void read(std::uint32_t off, std::uint8_t* data, std::size_t n) override {
        d_.readSdram(hipi::LT7683::kTextLogAddr + off, data, n);
    }
private:
    hipi::LT7683& d_;
};

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

std::string textOf(const hipi::DisplayText& t) {
    std::string s;
    t.read([&](const char* p, std::size_t n) { s.append(p, n); });
    return s;
}

void feed(hipi::DisplayText& t, const std::string& s, Model m) {
    for (unsigned char c : s) t.put(c, m);
}

// Shows what came out when it isn't what was expected
bool same(const std::string& got, const std::string& want) {
    if (got == want) return true;
    std::printf("     got:  \"");
    for (unsigned char c : got) { if (c == '\n') std::printf("\\n"); else std::printf("%c", c); }
    std::printf("\"\n     want: \"");
    for (unsigned char c : want) { if (c == '\n') std::printf("\\n"); else std::printf("%c", c); }
    std::printf("\"\n");
    return false;
}

std::string esc(const char* s) { return std::string("\x1b") + s; }
std::string at(int col, int row) {           // ESC % col row (the bytes may be 0)
    return std::string("\x1b%") + static_cast<char>(col) + static_cast<char>(row);
}

void filter82163() {
    VectorStore st(4096);
    hipi::DisplayText t(&st);
    t.setExtArgCmds("V");
    const Model m = Model::Hp82163;
    check(t.empty() && textOf(t).empty(), "82163: nothing yet");

    feed(t, "01 LBL \"AB\"\r\n02 SIN   \r\n", m);
    check(same(textOf(t), "01 LBL \"AB\"\n02 SIN\n"), "82163: CR LF ends a line, trailing spaces dropped");

    feed(t, "LF only\nand an empty line\r\n\r\nnext\r\n", m);
    check(same(textOf(t), "01 LBL \"AB\"\n02 SIN\nLF only\nand an empty line\n\nnext\n"),
          "82163: LF alone, empty lines kept");

    t.restart();
    feed(t, std::string("\xC9NVERSE") + "\r\n", m);
    check(same(textOf(t), "INVERSE\n"), "82163: bit 7 (inverse video) dropped");

    t.restart();
    feed(t, "TYPO\x08\x08PE\r\n", m);
    check(same(textOf(t), "TYPE\n"), "82163: BS takes the last character back");

    t.restart();
    feed(t, esc("<") + "A" + esc("C") + esc("D") + "B" + esc("#") + "V\x07" + "C" + esc("#") + "D" + "D\r\n", m);
    check(same(textOf(t), "ABCD\n"), "82163: escapes dropped, with ESC # V's argument");

    t.restart();
    feed(t, "TITLE" + at(5, 3) + "X=1" + at(0, 4) + "Y=2\r\n", m);
    check(same(textOf(t), "TITLE\nX=1\nY=2\n"), "82163: ESC % (with a 0 argument) ends the line so far");

    t.restart();
    feed(t, "first screen\r\n" + esc("E") + "second\r\n" + esc("E") + esc("E") + "third", m);
    check(same(textOf(t), "first screen\n\nsecond\n\nthird\n"),
          "82163: ESC E leaves one empty line; the line being written comes last");
    check(t.lines() == 5, "82163: lines() counts the line being written");

    t.restart();
    feed(t, "a\x01\x07\x7f" "b\r\n", m);
    check(same(textOf(t), "ab\n"), "82163: other control codes and DEL dropped");

    t.restart();
    feed(t, "before\r\n", m);
    t.cleared();
    feed(t, "after\r\n", m);
    check(same(textOf(t), "before\n\nafter\n"), "Clear device: an empty line");
    check(!st.bad, "store: offsets in range");
}

void filter92198() {
    VectorStore st(4096);
    hipi::DisplayText t(&st);
    t.setExtArgCmds("V");
    const Model m = Model::Hp92198;

    feed(t, std::string("\xC1SCII mode \x85x\r\n"), m);
    check(same(textOf(t), "ASCII mode x\n"), "92198: ASCII 160..255 = inverse, 128..159 dropped");

    t.restart();
    feed(t, esc("j") + "\xD0\xD8\xDA \xC2\xDE \xFC\r\n" + esc("k") + "\xC1\r\n", m);
    check(same(textOf(t), "\xC3\x85\xC3\x84\xC3\x96 \xC3\xB4\xC3\x9F \xE2\x96\x88\nA\n"),
          "92198: Roman-8 as UTF-8 (\xC3\x85\xC3\x84\xC3\x96 \xC3\xB4\xC3\x9F, the block); ESC k back to ASCII");

    t.restart();
    feed(t, esc("j") + "\xD0" "\x08" "\xD8" "\r\n", m);
    check(same(textOf(t), "\xC3\x84\n"), "92198: BS takes back a whole UTF-8 character");

    t.restart();
    const std::string longLine(100, 'L');
    feed(t, longLine + "\r\n", m);
    check(same(textOf(t), longLine + "\n"), "92198: a 100-character line stays one line");

    t.restart();
    feed(t, "screen one\r\n" + esc("H") + esc("J") + "screen two\r\n" + esc("H") + "top" + esc("K"), m);
    check(same(textOf(t), "screen one\n\nscreen two\ntop\n"),
          "92198: ESC H ESC J clears (an empty line), ESC H alone ends the line");

    t.restart();
    feed(t, "x" + esc("Y") + "a\x01" + esc("E") + "b" + esc("Z") + "c\r\n", m);
    check(same(textOf(t), "xabc\n"), "92198: monitor mode -- characters kept, ESC Z ends it");

    t.restart();
    feed(t, esc("j") + "\xD0" + esc("e") + "\xC1\r\n", m);
    check(same(textOf(t), "\xC3\x85\n\nA\n"), "92198: ESC e (hard reset) clears and goes back to ASCII");

    t.restart();
    std::string huge(1200, 'W');
    feed(t, huge + "\r\n", m);
    const std::string s = textOf(t);
    check(s.size() == 1203 && s[512] == '\n' && s[1025] == '\n', "a line longer than 512 is broken there");
}

void ring() {
    VectorStore st(1000);
    hipi::DisplayText t(&st);
    char buf[32];
    std::string want;
    for (int i = 1; i <= 300; ++i) {
        std::snprintf(buf, sizeof buf, "line %03d\r\n", i);
        feed(t, buf, Model::Hp82163);
    }
    const std::string s = textOf(t);
    check(t.overflowed(), "ring: 2700 bytes into 1000 -- run over");
    check(s.size() <= 1000 + 256 && s.size() >= 1000 - 9, "ring: about its size kept (+ what isn't written yet)");
    check(s.compare(0, 5, "line ") == 0, "ring: starts with a whole line");
    check(s.size() >= 9 && s.compare(s.size() - 9, 9, "line 300\n") == 0, "ring: the newest line is there");
    bool inOrder = true;
    int prev = 0;
    for (std::size_t p = 0; p + 9 <= s.size(); p += 9) {
        const int n = std::atoi(s.c_str() + p + 5);
        if (s.compare(p, 5, "line ") != 0 || (prev != 0 && n != prev + 1)) inOrder = false;
        prev = n;
    }
    check(inOrder, "ring: whole lines, in order");
    check(t.lines() == 300 && !st.bad, "ring: 300 lines counted, offsets in range");
}

void sdram(hipi::LT7683& d) {
    // Round trip through the chip, and the panel isn't touched
    std::vector<std::uint8_t> before(4096), after(4096);
    d.readSdram(0, before.data(), before.size());
    std::vector<std::uint8_t> data(3000), back(3000);
    for (std::size_t i = 0; i < data.size(); ++i) data[i] = static_cast<std::uint8_t>(i * 7 + 3);
    d.writeSdram(hipi::LT7683::kTextLogAddr + 100, data.data(), data.size());
    d.readSdram(hipi::LT7683::kTextLogAddr + 100, back.data(), back.size());
    check(back == data, "SDRAM: 3000 bytes written and read back");
    d.readSdram(0, after.data(), after.size());
    check(before == after, "SDRAM: the panel is not touched");
    check(hipi::LT7683::kTextLogAddr >= hipi::LT7683::kGooseAddr + hipi::LT7683::kGooseMaxBytes &&
          hipi::LT7683::kTextLogAddr + hipi::LT7683::kTextLogBytes <= 16UL * 1024 * 1024,
          "SDRAM: the text log after the geese, inside 16 MB");
}

// HP-IL to the display device, both models, the log in the chip's SDRAM
void throughTheDevice(hipi::LT7683& d) {
    ChipStore store(d);
    hipi::displayText().setStore(&store);
    hipi::displayText().setExtArgCmds("V");

    auto* term = new hipi::Video80(&d);
    screen = new hipi::Screen(&d, 0x07E0, 1, 200, 1024);
    screen->suspend();
    auto* dev = new CDisplay("TFDISPLAY");
    dev->setVideo80(term);
    dev->setEnabled(true);
    auto send = [&](IL_CMD_t f) { return dev->hpil(f); };
    auto out = [&](const std::string& s) {
        send(LAD + 1);
        for (unsigned char c : s) send(c);
        send(UNL);
        while (dev->fifoSize() > 0) dev->idle();
    };
    send(AAU);
    send(AAD + 1);

    out("10 DISP \"HELLO\"\r\n" + esc("%") + std::string("\x05\x02", 2) + "20 END\r\n");
    dev->setModel(DisplayModel::Hp92198);
    term->setActive(true);
    out(esc("j") + "\xD0" "ngstr\xCE" "m\r\n");
    // Clear device (DCL)
    send(DCL);
    out("after DCL\r\n");
    // Lots more, through more than one 256-byte piece
    for (int i = 0; i < 100; ++i) out("0123456789\r\n");
    const std::string s = textOf(hipi::displayText());
    std::string want = "10 DISP \"HELLO\"\n20 END\n\xC3\x85ngstr\xC3\xB6m\n\nafter DCL\n";
    for (int i = 0; i < 100; ++i) want += "0123456789\n";
    check(same(s, want), "HP-IL -> display device -> text log in SDRAM (82163, then 92198 in Roman-8, DCL)");
    hipi::displayText().setStore(nullptr);
}

}  // namespace

int main() {
    filter82163();
    filter92198();
    ring();

    ChipTransport t;
    hipi::LT7683 d(t, SCREEN_MAX_X, SCREEN_MAX_Y);
    d.begin();
    sdram(d);
    throughTheDevice(d);

    if (failures) {
        std::printf("%d FAILED\n", failures);
        return 1;
    }
    std::printf("all display text tests passed\n");
    return 0;
}
