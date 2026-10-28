// hwtest_view_test.cpp -- the Hardware test page (System -> Hardware
// test) as HIPI draws it: the real hwtest.cpp and views.cpp with the
// LT7683 driver against the emulated chip; the LEDs, pixels and I2C bus
// are a stand-in (hwio_stub.h) that remembers what the page did. Taps go
// to the page's buttons, the main loop is run, and the panel is saved:
//
//   hwtest_1_page.png      the page, with a made-up I2C bus
//   hwtest_5_hpil_all.png  HP-IL on LED 4: ALL
//   hwtest_2_running.png   a pixel running along, LED buttons
//   hwtest_3_scan.png      the i2cdetect grid (Scan I2C)
//
//   make hwtest_view_test && ./hwtest_view_test   (in tools/hipiview)
#include "LT7683.hpp"
#include "Screen.hpp"
#include "views.h"
#include "printer.h"
#include "hwtest.h"
#include "hwio_stub.h"
#include "hp82163_font.hpp"
#include "plotter.h"
#include "../lt7683_emu.hpp"
#include "../png.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

std::uint64_t fakeNowUs = 1000000;
bool bTrace = false, bExtTrace = false, usb_connected = true;
std::vector<CDevice*> devices;
CPlotter* plotter = nullptr;
CPrinter* printer = nullptr;
void hipi_bootService() {}
void hipi_bootServiceDone() {}

// The mirror isn't used here
namespace hipi {
void displayMirror_addLayer(std::uint32_t, std::uint32_t, std::uint32_t, std::uint16_t, std::uint32_t) {}
std::uint32_t displayMirror_fileAssetId(const char*, std::uint32_t) { return 1; }
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

void snapshot(const char* png) {
    chip.render(panel.data(), 0.0);
    writePng(png, panel.data(), 1024, 600);
    std::printf("     saved %s\n", png);
}

std::uint32_t at(int x, int y) { return panel[static_cast<std::size_t>(y) * 1024 + x] & 0xFFFFFF; }

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

// The main loop: polls for `ms` of (fake) time
void run(int ms) {
    for (int t = 0; t < ms; t += 5) {
        fakeNowUs += 5000;
        hipi::views_poll();
        hipi::hwtest_poll();
    }
}


// The middle of a button, as hwtest.cpp lays them out on the 7" panel
struct Pt { int x, y; };
Pt ledButton(int i) { return { 32 + i * 66 + 28, 116 + 21 }; }
constexpr Pt kChase { 32 + 48, 170 + 21 }, kLedOff { 32 + 108 + 36, 170 + 21 };
constexpr Pt kRun { 532 + 35, 178 + 18 }, kRed { 532 + 77 + 35, 178 + 18 }, kPixOff { 532 + 5 * 77 + 35, 178 + 18 };
constexpr Pt kPlus { 814 + 20, 64 + 18 }, kScan { 32 + 60, 296 + 18 }, kLedFitted { 32 + 66, 64 + 18 };
Pt orderButton(int i) { return { 620 + i * 62 + 20, 108 + 13 }; }

// A stand-in for the chip's built-in font: HIPI's own font for ASCII
void makeChipFont() {
    std::uint8_t f[256][16];
    for (int c = 0; c < 256; ++c) {
        if (c >= 32 && c < 127) std::memcpy(f[c], hipi::font[c - 32], 16);
        else for (int r = 0; r < 16; ++r) f[c][r] = (r == 2 || r == 13) ? 0x7E : (r > 2 && r < 13 ? 0x42 : 0);
    }
    chip.setBuiltinFont(f);
}

bool tap(Pt p) { return hipi::hwtest_tap(p.x, p.y); }
bool yellowAt(int x, int y) { const std::uint32_t c = at(x, y); return ((c >> 16) & 0xFF) > 150 && ((c >> 8) & 0xFF) > 100 && (c & 0xFF) < 60; }
int litPixels() {
    int n = 0;
    for (const auto& p : hwstub::pixels) if (p.r || p.g || p.b) ++n;
    return n;
}

hipi::I2cFound i2c(std::uint8_t a, hipi::I2cState s, const char* name, const char* what, const char* detail,
                   const char* file = "built in") {
    hipi::I2cFound f;
    f.addr = a;
    f.state = s;
    std::snprintf(f.name, sizeof(f.name), "%s", name);
    std::snprintf(f.what, sizeof(f.what), "%s", what);
    std::snprintf(f.detail, sizeof(f.detail), "%s", detail);
    std::snprintf(f.file, sizeof(f.file), "%s", s == hipi::I2cState::Unknown ? "" : file);
    return f;
}

}  // namespace

int main(int argc, char** argv) {
    const char* root = argc > 1 ? argv[1] : "../..";
    if (chdir(root) != 0) { std::perror(root); return 1; }

    ChipTransport t;
    hipi::LT7683 d(t, SCREEN_MAX_X, SCREEN_MAX_Y);
    d.begin();
    makeChipFont();
    hipi::Screen screen(&d, 0x07E0, 1, 200, 1024);
    plotter = new CPlotter("TFPLOT");
    printer = new CPrinter("TFPRINT");
    devices = { plotter, printer };
    hipi::views_setPrinter(printer);
    hipi::views_init(&d, &screen, plotter, &plotter->hpgl());
    hipi::hwtest_init(&d);

    using S = hipi::I2cState;
    hwstub::bus = {
        i2c(0x29, S::Ok,       "VL53L0X", "distance sensor", "ID 0xEE"),
        i2c(0x38, S::Ok,       "FT5316",  "touch panel", "in use", "built in"),
        i2c(0x3C, S::Probable, "SSD1306", "OLED display", ""),
        i2c(0x42, S::Unknown,  "Unknown", "", ""),
        i2c(0x57, S::Probable, "AT24C32", "EEPROM (clock module)", ""),
        i2c(0x68, S::Fault,    "DS3231",  "real-time clock", "00:00:12  24.25 C  time lost"),
        i2c(0x76, S::Ok,       "BMP280",  "temperature/pressure", "21.4 C  1013 hPa", "weather.txt"),
    };

    // ── The page opens ───────────────────────────────────────────────
    hipi::views_showHwTest();
    check(hipi::views_output() == hipi::DisplayOutput::HwTest, "System -> Hardware test: the page is shown");
    check(!hwstub::ledsTaken && !hwstub::pixelsTaken && hwstub::scans >= 1,
          "it only looks at the LEDs and pixels (nothing taken over) and scans the bus");
    run(2000);                                              // the name box goes away
    snapshot("tools/hipiview/hwtest_1_page.png");

    // ── Watching: what TFLEDS / TFPIXEL put on them shows on the page ──
    {
        hwstub::led[2] = true;                              // as TFLEDS "3O"
        hwstub::pixelCount = 8;                             // as TFPIXEL "0N8" ...
        const hwstub::Rgb cols[8] = { { 255, 0, 0 }, { 0, 255, 0 }, { 0, 0, 255 }, { 255, 255, 0 },
                                      { 0, 0, 0 }, { 255, 255, 255 }, { 40, 0, 0 }, { 255, 0, 255 } };
        for (int i = 0; i < 8; ++i) hwstub::pixels[static_cast<std::size_t>(i)] = cols[i];
        run(100);
        chip.render(panel.data(), 0.0);
        check(yellowAt(ledButton(2).x - 20, ledButton(2).y - 10) && !yellowAt(ledButton(1).x - 20, ledButton(1).y - 10),
              "TFLEDS lights LED 3: its button lit on the page");
        auto dot = [](int i) { return at(544 + (15 - i) * 29, 154); };   // (pixel 1 on the right)
        auto isRed = [](std::uint32_t c) { return ((c >> 16) & 0xFF) > 150 && ((c >> 8) & 0xFF) < 80 && (c & 0xFF) < 80; };
        auto isBlue = [](std::uint32_t c) { return ((c >> 16) & 0xFF) < 80 && (c & 0xFF) > 150; };
        check(isRed(dot(0)) && isBlue(dot(2)), "TFPIXEL's colours on the dots (pixel 1 red, pixel 3 blue)");
        check(((dot(6) >> 16) & 0xFF) > 40 && ((dot(6) >> 16) & 0xFF) < ((dot(0) >> 16) & 0xFF) - 60 && (dot(6) & 0xFFFF) < 0x1010,
              "... a dim one shown dimmer");
        check((dot(7) & 0xFFFFFF) != 0 && (dot(8) & 0xFFFFFF) == 0, "... 8 of them (TFPIXEL's count), the 9th unlit");
        snapshot("tools/hipiview/hwtest_4_watching.png");
        check(!hwstub::ledsTaken && !hwstub::pixelsTaken, "... and the page took nothing over");
        hwstub::led[2] = false;
        hwstub::pixelCount = 5;
        for (auto& p : hwstub::pixels) p = hwstub::Rgb{ 0, 0, 0 };
        run(100);
        chip.render(panel.data(), 0.0);
        check(!yellowAt(ledButton(2).x - 20, ledButton(2).y - 10) && (dot(0) & 0xFFFFFF) == 0, "TFLEDS / TFPIXEL off: off on the page too");
    }

    // ── LEDs ─────────────────────────────────────────────────────────
    check(tap(ledButton(0)) && hwstub::led[0] && hwstub::ledsTaken, "LED 1 button: a test -- the LEDs taken over, LED 1 on");
    chip.render(panel.data(), 0.0);
    check(yellowAt(ledButton(0).x - 20, ledButton(0).y - 10), "... and its button lit");
    check(tap(ledButton(0)) && !hwstub::led[0], "again: off");
    tap(kChase);
    int seen = 0;
    bool was[5] = {};
    for (int k = 0; k < 10; ++k) {
        run(150);
        int on = 0;
        for (int i = 0; i < 5; ++i) { if (hwstub::led[i]) { ++on; was[i] = true; } }
        if (on == 1) ++seen;
    }
    check(seen >= 8 && was[0] && was[3] && !was[4], "Chase (pixels fitted): one LED at a time, 1 to 4");
    tap(kLedOff);
    check(tap(ledButton(4)) && !hwstub::led[4], "pixels fitted: LED 5 (GPIO26, the strip's pin) does nothing");
    hwstub::pixelsFitted = false;
    tap(kChase);
    bool was5[5] = {};
    for (int k = 0; k < 10; ++k) {
        run(150);
        for (int i = 0; i < 5; ++i) if (hwstub::led[i]) was5[i] = true;
    }
    check(was5[0] && was5[4], "Chase (no pixels): 1 to 5");
    tap(kLedOff);
    check(tap(ledButton(4)) && hwstub::led[4], "no pixels: LED 5 works");
    tap(ledButton(4));
    hwstub::pixelsFitted = true;
    check(!hwstub::led[0] && !hwstub::led[1] && !hwstub::led[2] && !hwstub::led[3] && !hwstub::led[4], "Off: all off");
    tap(kLedFitted);
    check(!hwstub::ledsFitted && tap(ledButton(2)) && !hwstub::led[2], "Fitted: NO -- the LED buttons do nothing");
    tap(kLedFitted);
    // HP-IL on LED 4: Messages -> Off -> All -> Messages
    {
        const Pt kHpil { 32 + 132 + 12 + 104, 64 + 18 };
        hwstub::hpilLeds = 2;
        tap(kHpil);
        check(hwstub::hpilLeds == 0, "HP-IL on LED 4: MESSAGES -> OFF");
        tap(kHpil);
        check(hwstub::hpilLeds == 1, "... -> ALL");
        snapshot("tools/hipiview/hwtest_5_hpil_all.png");
        tap(kHpil);
        check(hwstub::hpilLeds == 2, "... -> MESSAGES");
    }
    // Plotter error on LED 3: ON -> OFF -> ON (beside Off, on the Chase row)
    {
        const Pt kPlot { 32 + 96 + 12 + 72 + 12 + 60, 170 + 21 };
        hwstub::plotterLed = true;
        tap(kPlot);
        check(!hwstub::plotterLed, "Plotter error on LED 3: ON -> OFF");
        tap(kPlot);
        check(hwstub::plotterLed, "... -> ON");
    }

    // ── Pixels ───────────────────────────────────────────────────────
    tap(kRed);
    check(litPixels() == 5 && hwstub::pixels[0].r > 0 && hwstub::pixels[0].g == 0, "Red: the 5 pixels red");
    tap(kPlus);
    check(hwstub::pixelCount == 6 && litPixels() == 6, "+: 6 pixels, the new one red too");
    check(hwstub::pixelOrder == 0, "colour order: RGB to start with");
    check(tap(orderButton(1)) && hwstub::pixelOrder == 1, "GRB chosen (radio button)");
    check(litPixels() == 6, "... the pixels still lit, sent again");
    chip.render(panel.data(), 0.0);
    check(yellowAt(orderButton(1).x - 11, orderButton(1).y) && !yellowAt(orderButton(0).x - 11, orderButton(0).y),
          "... GRB's button filled, RGB's not");
    tap(orderButton(0));
    check(hwstub::pixelOrder == 0, "back to RGB");
    tap(kRun);
    run(100);
    check(litPixels() == 1, "Run: one pixel lit");
    int firstLit = -1;
    for (int i = 0; i < 6; ++i) if (hwstub::pixels[static_cast<std::size_t>(i)].r || hwstub::pixels[static_cast<std::size_t>(i)].g || hwstub::pixels[static_cast<std::size_t>(i)].b) firstLit = i;
    run(60);
    int nextLit = -1;
    for (int i = 0; i < 6; ++i) if (hwstub::pixels[static_cast<std::size_t>(i)].r || hwstub::pixels[static_cast<std::size_t>(i)].g || hwstub::pixels[static_cast<std::size_t>(i)].b) nextLit = i;
    check(nextLit == (firstLit + 1) % 6, "... and it moves along");
    snapshot("tools/hipiview/hwtest_2_running.png");
    bool red = false, green = false, blue = false, white = false;
    for (int k = 0; k < 6 * 4 + 2; ++k) {
        run(60);
        for (const auto& p : hwstub::pixels) {
            if (p.r && !p.g && !p.b) red = true;
            if (p.g && !p.r && !p.b) green = true;
            if (p.b && !p.r && !p.g) blue = true;
            if (p.r && p.g && p.b) white = true;
        }
    }
    check(red && green && blue && white, "... a new colour every round: red, green, blue, white");
    tap(kPixOff);
    check(litPixels() == 0 && !hwstub::pixelsTaken, "Off: all off, the pixels given back (watched again)");

    // ── I2C ──────────────────────────────────────────────────────────
    const int before = hwstub::scans;
    run(3100);
    check(hwstub::scans > before, "the bus is looked at again every 3 s");
    tap(kScan);
    snapshot("tools/hipiview/hwtest_3_scan.png");
    chip.render(panel.data(), 0.0);
    check(yellowAt(SCREEN_MAX_X / 2 - 235 + 1, SCREEN_MAX_Y / 2), "Scan I2C: the grid box");
    tap({ 10, 590 });
    chip.render(panel.data(), 0.0);
    check(!yellowAt(SCREEN_MAX_X / 2 - 235 + 1, SCREEN_MAX_Y / 2), "a tap closes it");

    // ── Leaving: the close button (title bar, right) ─────────────────
    chip.render(panel.data(), 0.0);
    check(!yellowAt(SCREEN_MAX_X - 32, 13) && yellowAt(SCREEN_MAX_X - 120, 13), "the close button in the title bar");
    check(tap({ SCREEN_MAX_X - 30, 14 }), "the close button is touched");
    check(hipi::views_output() != hipi::DisplayOutput::HwTest && !hwstub::ledsTaken && !hwstub::pixelsTaken,
          "leaving: back to the view before, the hardware given back");
    check(!tap(ledButton(0)) && !hwstub::led[0], "taps do nothing once it's closed");

    std::printf("%s\n", failures ? "FAILED" : "ALL PASSED");
    return failures ? 1 : 0;
}
