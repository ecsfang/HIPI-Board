// clock_view_test.cpp -- the clock screen saver (clock.cpp, with its
// stand-in clock: HIPI_FAKE_RTC) drawn on the emulated chip: the colon
// blinks -- lit one second, dark the next -- and nothing else of the time
// and date changes within the minute; the minutes never go; a new minute
// appears at once (drawn off-screen, copied in one go); the minute's goose
// flies along the bottom. Saves clock_view_lit.png and clock_view_dark.png.
//
//   make clock_view_test && ./clock_view_test   (in tools/hipiview)
#include "LT7683.hpp"
#include "clock.h"
#include "config.hpp"
#include "views.h"
#include "../lt7683_emu.hpp"
#include "../png.hpp"
#include <cstdio>
#include <algorithm>
#include <cstdlib>
#include <vector>

std::uint64_t& fakeTimeUs() { static std::uint64_t t = 1000000; return t; }
// (the I2C bus: nothing on it -- the stand-in clock is used)
int fakeI2cWrite(std::uint8_t, const std::uint8_t*, std::size_t) { return -1; }
int fakeI2cRead(std::uint8_t, std::uint8_t*, std::size_t) { return -1; }
std::uint64_t fakeNowUs = 1000000;          // (saver_anim's stand-in clock)
hipi::Config config;
bool bTrace = false, bExtTrace = false;
namespace hipi {
DisplayOutput views_output() { return DisplayOutput::Clock; }
bool views_isSplashVisible() { return false; }
void displayMirror_addLayer(std::uint32_t, std::uint32_t, std::uint32_t, std::uint16_t, std::uint32_t) {}
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
void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}
std::vector<std::uint32_t> panel() {
    std::vector<std::uint32_t> px(1024 * 600);
    chip.render(px.data(), 0.0, false);
    return px;
}
// The columns where two pictures differ: [x0, x1), none: x0 = x1 = 0
constexpr int kAboveGoose = 480;            // the time and the date: above the goose's band
void diffColumns(const std::vector<std::uint32_t>& a, const std::vector<std::uint32_t>& b, int& x0, int& x1) {
    x0 = 1024; x1 = 0;
    for (int y = 0; y < kAboveGoose; ++y)
        for (int x = 0; x < 1024; ++x)
            if (a[y * 1024 + x] != b[y * 1024 + x]) { x0 = std::min(x0, x); x1 = std::max(x1, x + 1); }
    if (x1 == 0) x0 = 0;
}
// Lit pixels in the columns [x0, x1) (the clock: light on black)
int litIn(const std::vector<std::uint32_t>& p, int x0, int x1, int y0 = 0, int y1 = kAboveGoose) {
    int n = 0;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) n += (p[y * 1024 + x] & 0xFFFFFF) > 0x404040;
    return n;
}
}  // namespace

int main() {
    ChipTransport t;
    hipi::LT7683 d(t, SCREEN_MAX_X, SCREEN_MAX_Y);
    d.begin();
    hipi::clock_init(&d);
    hipi::clock_set(2026, 10, 28, 9, 15, 50);   // (a few seconds before a new minute)
    hipi::clock_setShow(hipi::SaverShow::Clock, false);
    hipi::clock_drawAll();
    auto step = [] { fakeTimeUs() += 10000; fakeNowUs = fakeTimeUs(); hipi::clock_poll(); };
    // Into the next second, just past its start
    for (int i = 0; i < 100; ++i) step();
    std::vector<std::vector<std::uint32_t>> secs;
    for (int s = 0; s < 8; ++s) {
        secs.push_back(panel());
        for (int i = 0; i < 100; ++i) step();       // a second on
    }
    int x0, x1;
    diffColumns(secs[0], secs[1], x0, x1);
    std::printf("     a second later: columns %d..%d change\n", x0, x1);
    check(x1 > x0 && x1 - x0 <= 52, "a second later only the colon changes (at most its 52 px)");
    const bool firstLit = litIn(secs[0], x0, x1) > litIn(secs[1], x0, x1);
    writePng("clock_view_lit.png", secs[firstLit ? 0 : 1].data(), 1024, 600);
    writePng("clock_view_dark.png", secs[firstLit ? 1 : 0].data(), 1024, 600);
    std::printf("     saved clock_view_lit.png, clock_view_dark.png\n");
    check(std::abs(litIn(secs[0], x0, x1) - litIn(secs[1], x0, x1)) > 100, "... lit one second, dark the next");
    bool alternates = true;
    for (std::size_t s = 2; s < secs.size(); ++s) {
        int a, b;
        diffColumns(secs[s - 2], secs[s], a, b);
        if (a != b) alternates = false;              // two seconds apart: the same picture
        diffColumns(secs[s - 1], secs[s], a, b);
        if (!(b > a && b - a <= 52)) alternates = false;
    }
    check(alternates, "lit a second, dark a second -- nothing else changes in the minute");
    // The minutes are there in every second (right of the colon)
    bool minutes = true;
    for (const auto& p : secs) if (litIn(p, x1, 1024) < 2000) minutes = false;
    check(minutes, "the minutes are shown all the time");
    // A new minute: drawn off-screen, then copied onto the panel in one go
    // (drawn straight onto it, the digits came down row by row)
    int timeCopies = 0;
    chip.onBteCopy = [&](std::uint32_t src, std::uint32_t dst, int, int, int w, int h) {
        if (src == hipi::LT7683::kBteLayer3Addr && dst == 0 && w == 1024 && h >= 150) ++timeCopies;
    };
    for (int i = 0; i < 300; ++i) step();
    chip.onBteCopy = nullptr;
    check(timeCopies == 1, "a new minute: the time copied in at once (drawn off-screen)");
    const auto m1 = panel();
    for (int i = 0; i < 100; ++i) step();
    const auto m2 = panel();
    int a, b;
    diffColumns(m1, m2, a, b);
    check(b > a && b - a <= 52 && litIn(m2, b, 1024) > 2000, "after the minute changed: still only the colon blinks, minutes shown");
    // The goose: in the band below, moving right with the seconds
    auto gooseX = [](const std::vector<std::uint32_t>& p) {          // its left edge, -1: none on screen
        for (int x = 0; x < 1024; ++x)
            for (int y = kAboveGoose; y < 600; ++y)
                if ((p[y * 1024 + x] & 0xFFFFFF) > 0x404040) return x;
        return -1;
    };
    const int g1 = gooseX(m2);
    for (int i = 0; i < 1000; ++i) step();      // 10 s on
    const auto m3 = panel();
    const int g2 = gooseX(m3);
    std::printf("     the goose at x %d, 10 s later at x %d\n", g1, g2);
    check(g1 >= 0 && g2 > g1 + 150 && g2 < g1 + 210, "the goose flies right, about 18 px a second (a minute across)");
    // :30 -- in the middle (its centre)
    hipi::clock_set(2026, 10, 28, 9, 20, 30);
    hipi::clock_set(2026, 10, 28, 9, 21, 29);
    for (int i = 0; i < 150; ++i) step();       // :30, half a second in
    const auto mid = panel();
    // (the middle of its ink; at :30.5 it's half a second -- 9 px -- past the centre)
    int inkL = 1024, inkR = -1;
    for (int x = 0; x < 1024; ++x)
        for (int y = kAboveGoose; y < 600; ++y)
            if ((mid[y * 1024 + x] & 0xFFFFFF) > 0x404040) { inkL = std::min(inkL, x); inkR = std::max(inkR, x); }
    const int centre = (inkL + inkR + 1) / 2;   // (the speed: (1024 + ink) / 60 px a second)
    std::printf("     at :30.5 the goose's ink %d..%d, its middle at x %d (screen centre 512 + half a second)\n", inkL, inkR, centre);
    check(std::abs(centre - 521) <= 2, "at :30 it is in the middle of the screen");
    // :59.9 -- almost gone on the right; :00 -- just coming in on the left
    hipi::clock_set(2026, 10, 28, 9, 22, 58);
    for (int i = 0; i < 199; ++i) step();       // :59.98
    const int gEnd = gooseX(panel());
    for (int i = 0; i < 2; ++i) step();         // :00.0
    const auto start = panel();
    const int gStart = gooseX(start);
    std::printf("     at :59.98 its left edge at x %d; at :00 at x %d\n", gEnd, gStart);
    check(gEnd > 1024 - 3 || gEnd < 0, "at :59.98 it has just left on the right (its ink, or nothing, at the very edge)");
    check(gStart < 0 || gStart <= 1, "at :00 it just comes in on the left");
    writePng("clock_view_goose.png", mid.data(), 1024, 600);

    std::printf("%s\n", failures ? "FAILED" : "ALL PASSED");
    return failures ? 1 : 0;
}
