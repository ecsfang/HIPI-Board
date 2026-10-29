// page_chrome_test.cpp -- the pages (views_isPage(), page_chrome.h) on the
// PC: the Analyzer and the Character table, drawn by their real code on the
// emulated chip -- each with the close button (X) at the right of its title
// bar and the menu button in the bottom right corner, and nothing of the
// page in that corner (it's where the button strip comes from). Saves:
//
//   page_analyzer.png    the analyzer with a screenful of lines
//   page_chartable.png   the HP-41 character table
//   page_chardetails.png, page_chardetails_A.png   a character's details box
//
//   make page_chrome_test && ./page_chrome_test   (in tools/hipiview)
#include "LT7683.hpp"
#include "analyzer.h"
#include "chartable.h"
#include "page_chrome.h"
#include "views.h"
#include "hpil.h"
#include "hp82163_font.hpp"
#include "../lt7683_emu.hpp"
#include "../png.hpp"
#include <cstdio>
#include <cstring>
#include <vector>

std::uint64_t fakeNowUs = 1000000;
bool bTrace = false, bExtTrace = false;
std::vector<CDevice*> devices;           // (no devices of HIPI's: the analyzer names addresses)
namespace hipi {
DisplayOutput shownOutput = DisplayOutput::Analyzer;
DisplayOutput views_output() { return shownOutput; }
bool views_isSplashVisible() { return false; }
}

namespace {

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

int failures = 0;
std::vector<std::uint32_t> panel(1024 * 600);
void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}
void snapshot(const char* png) {
    chip.render(panel.data(), 0.0);
    writePng(png, panel.data(), 1024, 600);
    std::printf("     saved %s\n", png);
}
void makeChipFonts() {
    std::uint8_t f[256][16];
    for (int c = 0; c < 256; ++c) {
        if (c >= 32 && c < 127) std::memcpy(f[c], hipi::font[c - 32], 16);
        else std::memset(f[c], 0, 16);
    }
    chip.setBuiltinFont(f);
    static std::uint16_t f24[256][24];
    for (int c = 0; c < 256; ++c)
        for (int r = 0; r < 24; ++r) {
            std::uint16_t row = 0;
            for (int x = 0; x < 12; ++x)
                if (f[c][r * 16 / 24] & (0x80 >> (x * 8 / 12))) row = static_cast<std::uint16_t>(row | (0x8000 >> x));
            f24[c][r] = row;
        }
    chip.setBuiltinFont24(f24);
}
// The close button: white strokes in a black box at the title bar's right
bool closeDrawn() {
    int white = 0, black = 0;
    for (int y = 3; y < 18; ++y)
        for (int x = SCREEN_MAX_X - hipi::kPageCloseW + 8; x < SCREEN_MAX_X - 4; ++x) {
            const std::uint32_t v = panel[static_cast<std::size_t>(y) * 1024 + x] & 0xFFFFFF;
            if (v == 0xFFFFFF) ++white; else if (v == 0) ++black;
        }
    return white > 20 && black > 300;
}
// The menu button: its yellow frame and bars; around it (its touch area
// outside the button) nothing but black
bool menuDrawnAlone() {
    int yellow = 0, other = 0;
    for (int y = hipi::kPageMenuTouchY; y < SCREEN_MAX_Y; ++y)
        for (int x = hipi::kPageMenuTouchX; x < SCREEN_MAX_X; ++x) {
            const std::uint32_t v = panel[static_cast<std::size_t>(y) * 1024 + x] & 0xFFFFFF;
            const bool inButton = x >= hipi::kPageMenuX && x < hipi::kPageMenuX + hipi::kPageMenuW &&
                                  y >= hipi::kPageMenuY && y < hipi::kPageMenuY + hipi::kPageMenuH;
            if (inButton) { if (v != 0 && v != 0x101010 && (v >> 16) > 0xC0) ++yellow; }
            else if (v != 0) ++other;
        }
    return yellow > 200 && other == 0;
}

}  // namespace

int main() {
    ChipTransport t;
    hipi::LT7683 d(t, SCREEN_MAX_X, SCREEN_MAX_Y);
    d.begin();
    makeChipFonts();
    check(hipi::page_closeHit(1000, 10) && hipi::page_closeHit(965, 35) && !hipi::page_closeHit(900, 10) &&
          !hipi::page_closeHit(1000, 60), "page_closeHit(): the X (and a little below), nothing else");
    check(hipi::page_menuHit(1010, 590) && hipi::page_menuHit(950, 560) && !hipi::page_menuHit(900, 590) &&
          !hipi::page_menuHit(1010, 500), "page_menuHit(): the menu button's corner, nothing else");

    // ── The analyzer: lines long enough to reach the right edge ─────────
    hipi::analyzer_init(&d);
    hipi::analyzer_setMode(hipi::AnalyzerMode::Detailed);
    for (int i = 0; i < 400; ++i) {
        hipi::analyzer_capture(static_cast<IL_CMD_t>(0x420 + (i % 30)), static_cast<IL_CMD_t>(0x420 + (i % 30)));
        hipi::analyzer_capture(static_cast<IL_CMD_t>(0x41 + (i % 26)), static_cast<IL_CMD_t>(0x41 + (i % 26)));
        fakeNowUs += 3000;
    }
    for (int i = 0; i < 20; ++i) { fakeNowUs += 100000; hipi::analyzer_poll(); }
    hipi::analyzer_redrawAll();
    hipi::analyzer_poll();
    snapshot("page_analyzer.png");
    chip.render(panel.data(), 0.0, false);
    int lit = 0;
    for (int y = 30; y < 500; ++y)
        for (int x = 0; x < 600; ++x) lit += (panel[static_cast<std::size_t>(y) * 1024 + x] & 0xFFFFFF) != 0;
    check(lit > 5000, "the analyzer: lines shown");
    check(closeDrawn(), "... the X at the right of its title bar");
    check(menuDrawnAlone(), "... the menu button bottom right, no text in its corner");

    // ── The character table ─────────────────────────────────────────────
    hipi::shownOutput = hipi::DisplayOutput::CharTable;
    hipi::chartable_init(&d);
    hipi::chartable_drawAll();
    snapshot("page_chartable.png");
    chip.render(panel.data(), 0.0, false);
    check(closeDrawn(), "the character table: the X at the right of its title bar");
    check(menuDrawnAlone(), "... the menu button bottom right, the table ends above it");

    // ── A character touched: its details box ─────────────────────────────
    // (the cells: 64 px wide, 65 high from y 30 -- code 127 bottom right)
    auto whiteIn = [&](int x0, int y0, int x1, int y1) {
        chip.render(panel.data(), 0.0, false);
        int n = 0;
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x) n += (panel[static_cast<std::size_t>(y) * 1024 + x] & 0xFFFFFF) == 0xFFFFFF;
        return n;
    };
    const int charBox[4] = { 156, 120, 456, 410 };      // (the large character's place in the box)
    check(!hipi::chartable_tap(500, 15) && !hipi::chartable_detailsOpen(), "a touch in the title bar: no box");
    check(hipi::chartable_tap(15 * 64 + 30, 30 + 7 * 65 + 30) && hipi::chartable_selected() == 127,
          "a touch on 127: its details box");
    snapshot("page_chardetails.png");
    const int big127 = whiteIn(charBox[0], charBox[1], charBox[2], charBox[3]);
    check(closeDrawn() && menuDrawnAlone(), "... the page's X and menu button still there");
    hipi::chartable_step(1);
    check(hipi::chartable_selected() == 0, "> after 127: 0");
    hipi::chartable_step(-1);
    hipi::chartable_step(-62);                // 65: 'A'
    const int bigA = whiteIn(charBox[0], charBox[1], charBox[2], charBox[3]);
    snapshot("page_chardetails_A.png");
    hipi::chartable_step(-33);                // 32: space -- nothing lit
    const int bigSpace = whiteIn(charBox[0], charBox[1], charBox[2], charBox[3]);
    hipi::chartable_step(-30);                // 2: not shown -- every segment
    const int bigAll = whiteIn(charBox[0], charBox[1], charBox[2], charBox[3]);
    std::printf("     lit pixels: 127 %d, 'A' %d, space %d, all segments %d\n", big127, bigA, bigSpace, bigAll);
    check(bigSpace == 0 && big127 > 3000 && bigA > big127 && bigAll > bigA,
          "the large character: only its lit segments (space none, A more than |-, all segments most)");
    check(hipi::chartable_tap(30, 560) && !hipi::chartable_detailsOpen(), "a touch beside the box: closed");
    hipi::chartable_tap(5 * 64 + 30, 30 + 4 * 65 + 30);      // 69
    check(hipi::chartable_selected() == 69, "a touch on 69: its box");
    check(hipi::chartable_tap(132 + 352 + 130 + 58, 64 + 436 - 62 + 20) && !hipi::chartable_detailsOpen(), "CLOSE: closed");

    std::printf("%s\n", failures ? "FAILED" : "ALL PASSED");
    return failures ? 1 : 0;
}
