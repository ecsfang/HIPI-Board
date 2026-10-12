// printer_view_test.cpp -- the Printer view as HIPI draws it: the real
// plotterview.cpp, printer.cpp, BMP loader and LT7683 driver against the
// emulated chip, with the real pictures from resources/. The controller's
// frames go to the printer, plotterview_poll() runs as the main loop
// would, and the panel is saved as PNGs to look at:
//
//   printer_view_1_feeding.png   in the middle of the paper moving up
//   printer_view_1b_head.png     the head gliding over, printing a line
//   printer_view_2_printed.png   after a listing and some results
//   printer_view_3_hp41.png      HP-41 characters, double wide, graphics
//
// Checks: the paper outside the print area is untouched, the tear-off
// bar is never inked, and the panel shows ink where lines were printed.
//
//   make printer_view_test && ./printer_view_test   (in tools/hipiview)
#include "LT7683.hpp"
#include "Screen.hpp"
#include "plotterview.h"
#include "printer.h"
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
int lum(std::uint32_t c) { return static_cast<int>(((c >> 16) & 0xFF) + ((c >> 8) & 0xFF) + (c & 0xFF)) / 3; }

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

void send(const std::string& s) {
    printer->hpil(LAD + 1);
    for (unsigned char c : s) printer->hpil(c);
    printer->hpil(UNL);
}

}  // namespace

int main(int argc, char** argv) {
    // The pictures, as on the SD card
    const char* root = argc > 1 ? argv[1] : "../..";
    if (chdir(root) != 0) { std::perror(root); return 1; }

    ChipTransport t;
    hipi::LT7683 d(t, SCREEN_MAX_X, SCREEN_MAX_Y);
    d.begin();
    hipi::Screen screen(&d, 0x07E0, 1, 200, 1024);

    plotter = new CPlotter("TFPLOT");
    printer = new CPrinter("TFPRINT");
    printer->setEnabled(true);
    printer->hpil(AAD + 1);
    devices = { plotter, printer };
    hipi::plotterview_setPrinter(printer);
    hipi::plotterview_init(&d, &screen, plotter);
    check(hipi::plotterview_isAvailable(hipi::DisplayOutput::Printer), "Printer view loaded (hp82162a.bmp, prtparts.bmp)");
    hipi::plotterview_showDevice(printer);
    run(2000);                                              // the name box goes away

    chip.render(panel.data(), 0.0);
    const std::vector<std::uint32_t> blank = panel;         // the paper before printing

    // A program listing, as PRP sends it (8-bit mode, HP-41 characters)
    send("\x1b|");
    const char* listing[] = { "01*LBL \"FIB\"", "02 0", "03 STO 01", "04 1", "05 STO 02",
                              "06*LBL 01", "07 RCL 01", "08 RCL 02", "09 +", "10 X<>Y",
                              "11 STO 01", "12 VIEW X", "13 GTO 01", "14 END" };
    for (const char* l : listing) {
        send(std::string(l) + "\r");
        run(320);                                           // one at a time: animated
    }
    // A line: the head glides over first, then the paper brings it up
    chip.render(panel.data(), 0.0);
    const std::vector<std::uint32_t> beforeLine = panel;
    send("\xE8" "1.0000  ***\r");
    run(90);                                                // the head half way
    snapshot("tools/hipiview/printer_view_1b_head.png");
    {
        int slot = 0, bottom = 0;
        for (int y = 347; y < 357; ++y)
            for (int x = 560; x < 660; ++x) if (at(x, y) != (beforeLine[static_cast<std::size_t>(y) * 1024 + x] & 0xFFFFFF)) ++slot;
        int by0 = 9999, by1 = -1;
        for (int y = 300; y < 347; ++y)
            for (int x = 470; x < 770; ++x)
                if (at(x, y) != (beforeLine[static_cast<std::size_t>(y) * 1024 + x] & 0xFFFFFF)) {
                    ++bottom; by0 = std::min(by0, y); by1 = std::max(by1, y);
                }
        if (bottom) std::printf("     (changed: %d pixels, rows %d..%d)\n", bottom, by0, by1);
        check(slot > 50, "the print head glides along the slot");
        check(bottom == 0, "while it prints, the line can't be seen yet");
    }
    run(150);                                               // the paper moving up
    snapshot("tools/hipiview/printer_view_1_feeding.png");
    run(300);
    send("\xE8" "2.0000  ***\r" "\xE8" "3.0000  ***\r" "\xE8" "5.0000  ***\r");   // faster than the animation
    run(500);
    snapshot("tools/hipiview/printer_view_2_printed.png");

    check(printer->lineCount() == 18, "18 lines printed");
    // Ink where the last line is (bottom of the paper), right-justified
    int darkBottom = 0, darkBar = 0, changedOutside = 0;
    for (int y = 331; y < 347; ++y)
        for (int x = 488; x < 748; ++x) if (lum(at(x, y)) < 120) ++darkBottom;
    for (int y = 290; y < 310; ++y)
        for (int x = 470; x < 770; ++x)
            if (at(x, y) != (blank[static_cast<std::size_t>(y) * 1024 + x] & 0xFFFFFF)) ++darkBar;
    for (int y = 0; y < 600; ++y)
        for (int x = 0; x < 1024; ++x) {
            const bool inPaper = (x >= 496 && x < 748 && y >= 24 && y < 347) ||
                                 (x >= 488 && x < 748 && y >= 310 && y < 347) ||   // (slanted)
                                 (x >= 470 && x < 760 && y >= 346 && y < 358);   // (the head's slot)
            if (!inPaper && at(x, y) != (blank[static_cast<std::size_t>(y) * 1024 + x] & 0xFFFFFF)) ++changedOutside;
        }
    check(darkBottom > 100, "the newest line is at the print head");
    check(darkBar == 0, "the tear-off bar is not inked");
    check(changedOutside == 0, "nothing changed outside the print area");
    // Right-justified "5.0000  ***": its stars end at the right edge
    int rightInk = 0;
    for (int y = 331; y < 347; ++y)
        for (int x = 716; x < 748; ++x) if (lum(at(x, y)) < 160) ++rightInk;
    check(rightInk > 20, "right justified to the edge");

    // HP-41 characters, lower case, double wide, graphics columns
    std::string hp;
    for (int c = 0; c < 24; ++c) hp += static_cast<char>(c == 13 ? 'x' : c);
    send("\xE0" + hp + "\r");                               // (left again)
    send("\xD1" "LOWER CASE" "\r" "\xD0");
    send("\xD4" "DOUBLE" "\r" "\xD0");
    std::string g = "\xD2";
    for (int x = 0; x < 168; ++x) {
        const double v = 3.5 + 3.4 * std::sin(x * 6.2832 / 84.0);
        g += static_cast<char>(1 << static_cast<int>(v + 0.5));
    }
    send(g + "\xD0" "\r");
    printer->pressAdvance(true);                            // PAPER ADVANCE (one line)
    printer->pressAdvance(false);
    send("SIN(X)\r");
    run(1500);
    snapshot("tools/hipiview/printer_view_3_hp41.png");
    check(printer->lineCount() == 24, "24 lines printed");

    // Away and back: the paper is drawn again as it was
    chip.render(panel.data(), 0.0);
    const std::vector<std::uint32_t> before = panel;
    hipi::plotterview_cycleOutput(true);
    run(2000);
    hipi::plotterview_showDevice(printer);
    run(2000);
    chip.render(panel.data(), 0.0);
    long diff = 0;
    for (std::size_t i = 0; i < panel.size(); ++i) if (panel[i] != before[i]) ++diff;
    check(diff == 0, "the view comes back the same");

    std::printf("%s\n", failures ? "FAILED" : "ALL PASSED");
    return failures ? 1 : 0;
}
