// clock_view_test.cpp -- the clock screen saver (clock.cpp, with its
// stand-in clock: HIPI_FAKE_RTC) drawn on the emulated chip: the colon
// blinks -- lit one second, dark the next -- and nothing else changes
// within the minute; the minutes never go. Saves clock_view_lit.png and
// clock_view_dark.png.
//
//   make clock_view_test && ./clock_view_test   (in tools/hipiview)
#include "LT7683.hpp"
#include "clock.h"
#include "config.hpp"
#include "views.h"
#include "../lt7683_emu.hpp"
#include "../png.hpp"
#include <cstdio>
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
void diffColumns(const std::vector<std::uint32_t>& a, const std::vector<std::uint32_t>& b, int& x0, int& x1) {
    x0 = 1024; x1 = 0;
    for (int y = 0; y < 600; ++y)
        for (int x = 0; x < 1024; ++x)
            if (a[y * 1024 + x] != b[y * 1024 + x]) { x0 = std::min(x0, x); x1 = std::max(x1, x + 1); }
    if (x1 == 0) x0 = 0;
}
// Lit pixels in the columns [x0, x1) (the clock: light on black)
int litIn(const std::vector<std::uint32_t>& p, int x0, int x1) {
    int n = 0;
    for (int y = 0; y < 600; ++y)
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
    // A new minute: drawn, and the colon goes on blinking
    for (int i = 0; i < 300; ++i) step();
    const auto m1 = panel();
    for (int i = 0; i < 100; ++i) step();
    const auto m2 = panel();
    int a, b;
    diffColumns(m1, m2, a, b);
    check(b > a && b - a <= 52 && litIn(m2, b, 1024) > 2000, "after the minute changed: still only the colon blinks, minutes shown");
    std::printf("%s\n", failures ? "FAILED" : "ALL PASSED");
    return failures ? 1 : 0;
}
