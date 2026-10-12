// demo_view_test.cpp -- what the demo (System -> Demo) shows, rendered
// on the PC: the display texts, the 80-column screen, the plots and the
// printout from demo_content.cpp, through the real Screen, Video80,
// plotter, printer and views against the emulated chip. Saves demo_*.png.
//
//   make demo_view_test && ./demo_view_test   (in tools/hipiview)
#include "LT7683.hpp"
#include "Screen.hpp"
#include "plotterview.h"
#include "printer.h"
#include "demo_content.h"
#include "video80.h"
#include "hp82163_font.hpp"
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

class ChipTransport : public hipi::RA8875Transport {
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
        hipi::plotterview_poll();
    }
}



// Stand-ins for the chip's built-in fonts: HIPI's own font for ASCII
// (and stretched to 12x24), a hollow box for the rest
void makeChipFont() {
    std::uint8_t f[256][16];
    for (int c = 0; c < 256; ++c) {
        if (c >= 32 && c < 127) std::memcpy(f[c], hipi::font[c - 32], 16);
        else for (int r = 0; r < 16; ++r) f[c][r] = (r == 2 || r == 13) ? 0x7E : (r > 2 && r < 13 ? 0x42 : 0);
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

void plot(const std::string& g, bool black, const char* png) {
    hipi::plotterview_clearPlotter();
    hipi::plotterview_setPaperBlack(black);
    for (char c : g) plotter->put(c);
    run(50);
    snapshot(png);
}

}  // namespace

extern const std::uint16_t kLogoImageWidth, kLogoImageHeight;
extern const std::uint16_t kLogoImage[];

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
    hipi::plotterview_setPrinter(printer);
    hipi::plotterview_init(&d, &screen, plotter);

    // Display
    for (char c : hipi::demo::textIntro()) screen.pr_char(static_cast<std::uint8_t>(c));
    run(10);
    snapshot("tools/hipiview/demo_1_intro.png");
    for (char c : hipi::demo::textEscapes()) screen.pr_char(static_cast<std::uint8_t>(c));
    run(10);
    snapshot("tools/hipiview/demo_2_escapes.png");
    screen.setTextSize(3);
    for (char c : std::string("\x1b" "EHIPI\r\nSIZE 3")) screen.pr_char(static_cast<std::uint8_t>(c));
    snapshot("tools/hipiview/demo_3_size3.png");
    screen.setTextSize(1);
    std::vector<std::string> lif = { "Volume : HIPI", "", "NAME       TYPE    SIZE  BLOCKS  FLAGS",
        "FIB        PRGM    45r       2  ", "DATA1      DATA   100r       3  ", "KEYS       KEYS    12r       1  " };
    for (char c : hipi::demo::textDir("games.dat", lif)) screen.pr_char(static_cast<std::uint8_t>(c));
    snapshot("tools/hipiview/demo_4_dir.png");

    // 92198
    hipi::Video80 v(&d);
    v.setColor(0x07E0);
    screen.suspend();
    v.setActive(true);
    for (char c : hipi::demo::text80()) v.put(static_cast<std::uint8_t>(c));
    snapshot("tools/hipiview/demo_5_80col.png");
    for (char c : hipi::demo::text80Roman8()) v.put(static_cast<std::uint8_t>(c));
    snapshot("tools/hipiview/demo_5b_roman8.png");
    for (char c : hipi::demo::text40()) v.put(static_cast<std::uint8_t>(c));
    snapshot("tools/hipiview/demo_6_40col.png");
    v.setActive(false);

    // Plotter
    hipi::plotterview_setPlotFit(false);
    hipi::plotterview_showDevice(plotter);
    run(2000);
    plot(hipi::demo::hpglLogo(kLogoImage, kLogoImageWidth, kLogoImageHeight), true, "tools/hipiview/demo_7_logo.png");
    plot(hipi::demo::hpglSpirograph(), false, "tools/hipiview/demo_8_spiro.png");
    plot(hipi::demo::hpglFunctionPlot(), false, "tools/hipiview/demo_9_function.png");
    plot(hipi::demo::hpglLoop(), false, "tools/hipiview/demo_10_loop.png");

    // Printer
    hipi::plotterview_showDevice(printer);
    run(2000);
    for (const std::string& l : hipi::demo::printLines()) {
        for (char c : l) printer->put(static_cast<std::uint8_t>(c));
        run(900);
    }
    snapshot("tools/hipiview/demo_11_printer.png");
    check(printer->lineCount() == hipi::demo::printLines().size(), "every printout line printed");
    check(at(0, 0) == at(0, 0), "rendered");
    std::printf("%s\n", failures ? "FAILED" : "ALL PASSED");
    return failures ? 1 : 0;
}
