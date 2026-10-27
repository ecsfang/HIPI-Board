// printer_view_test.cpp -- the Printer view as HIPI draws it: the real
// views.cpp, printer.cpp, BMP loader and LT7683 driver against the
// emulated chip, with the real pictures from resources/. The controller's
// frames go to the printer, views_poll() runs as the main loop
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
#include "views.h"
#include "hp82163_font.hpp"
#include "boardui.h"            // (the pvstubs one: counts boardui_closePipBoxes())
#include "printout.h"
#include "printer.h"
#include "plotter.h"
#include "../lt7683_emu.hpp"
#include "../png.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <fstream>
#include <cstdlib>
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
int lum(std::uint32_t c) { return static_cast<int>(((c >> 16) & 0xFF) + ((c >> 8) & 0xFF) + (c & 0xFF)) / 3; }

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

// The main loop: polls for `ms` of (fake) time
void run(int ms) {
    for (int t = 0; t < ms; t += 5) {
        fakeNowUs += 5000;
        hipi::views_poll();
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
    {   // the chip's 8x16 font, for text drawn on the panel (the digitize panel)
        std::uint8_t f[256][16] = {};
        for (int c = 32; c < 127; ++c) std::memcpy(f[c], hipi::font[c - 32], 16);
        chip.setBuiltinFont(f);
    }
    hipi::Screen screen(&d, 0x07E0, 1, 200, 1024);

    plotter = new CPlotter("TFPLOT");
    printer = new CPrinter("TFPRINT");
    printer->setEnabled(true);
    printer->hpil(AAD + 1);
    devices = { plotter, printer };
    hipi::views_setPrinter(printer);
    hipi::views_init(&d, &screen, plotter);
    check(hipi::views_isAvailable(hipi::DisplayOutput::Printer), "Printer view loaded (hp82162a.bmp, prtparts.bmp)");
    const int closedBefore = hipi::pipBoxesClosedStub();
    hipi::views_showDevice(printer);
    // The name box takes PIP-1: an open info box / device list is closed first
    check(hipi::pipBoxesClosedStub() == closedBefore + 1, "view switch closes the PIP-1 boxes first");
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
    hipi::views_cycleOutput(true);
    run(2000);
    hipi::views_showDevice(printer);
    run(2000);
    chip.render(panel.data(), 0.0);
    long diff = 0;
    for (std::size_t i = 0; i < panel.size(); ++i) if (panel[i] != before[i]) ++diff;
    check(diff == 0, "the view comes back the same");

    // ── The paper moved by hand ────────────────────────────────────────
    for (int i = 0; i < 40; ++i) send("LINE " + std::to_string(i) + "\r");
    run(3000);
    auto inkRows = [](int y0, int y1) {
        chip.render(panel.data(), 0.0);
        int n = 0;
        for (int y = y0; y < y1; ++y)
            for (int x = 496; x < 748; ++x) if (lum(at(x, y)) < 120) ++n;
        return n;
    };
    check(!hipi::views_paperDragBegin(200, 300), "a finger beside the paper: no drag");
    check(hipi::views_paperDragBegin(620, 200), "a finger on the paper: it drags");
    // Up as far as it goes: the newest line out above the tear-off bar
    for (int y = 200; y >= 0; y -= 10) { hipi::views_paperDragMove(620, y); run(30); }
    hipi::views_paperDragEnd();
    snapshot("tools/hipiview/printer_view_4_pulled_up.png");
    check(hipi::views_paperScrolled(), "moved: not where it prints");
    check(inkRows(270, 290) > 50 && inkRows(310, 340) == 0, "pulled up: the newest line just above the tear-off bar, nothing below");
    const std::vector<std::uint32_t> upTop = panel;
    hipi::views_paperDragBegin(620, 100);
    hipi::views_paperDragMove(620, 20);                    // further up: no
    run(30);
    hipi::views_paperDragEnd();
    chip.render(panel.data(), 0.0);
    check(panel == upTop, "... and no further");
    // Down to the first line
    hipi::views_paperDragBegin(620, 40);
    for (int y = 40; y < 340; y += 7) { hipi::views_paperDragMove(620, y); run(20); }
    for (int k = 0; k < 4; ++k) {
        hipi::views_paperDragEnd();
        hipi::views_paperDragBegin(620, 40);
        for (int y = 40; y < 340; y += 13) { hipi::views_paperDragMove(620, y); run(20); }
    }
    hipi::views_paperDragEnd();
    snapshot("tools/hipiview/printer_view_5_first_line.png");
    const std::vector<std::uint32_t> stepped = panel;
    check(inkRows(24, 40) > 50, "pulled down: the first line at the top of the window");
    // The same place drawn afresh (one big jump): the same paper
    hipi::views_paperToLive();
    run(1500);
    check(!hipi::views_paperScrolled(), "X: back to the print head (gliding)");
    hipi::views_paperDragBegin(620, 30);
    hipi::views_paperDragMove(620, 30 + 2000);             // one jump, clamped to the first line
    hipi::views_paperDragEnd();
    chip.render(panel.data(), 0.0);
    long d2 = 0;
    for (std::size_t i = 0; i < panel.size(); ++i) if (panel[i] != stepped[i]) ++d2;
    check(d2 == 0, "moved step by step or at once: the same paper");
    // ▲ / ▼ (a line)
    hipi::views_paperScrollBy(-1);
    check(hipi::views_paperScrolled(), "▼: a line towards the newest");
    // Printing while it's moved: back to the head first, then the line
    send("AFTER\r");
    run(1500);
    snapshot("tools/hipiview/printer_view_6_printing_again.png");
    check(!hipi::views_paperScrolled() && inkRows(331, 347) > 50, "printing again: back at the head, the new line there");
    // ── Saving the paper (printouts/printout_<n>.bmp + .txt) ───────────
    {
        char cwd[512];
        if (getcwd(cwd, sizeof(cwd)) == nullptr) return 1;
        const std::string tmp = std::string(cwd) + "/tools/hipiview/printout_sd";
        if (std::system(("rm -rf '" + tmp + "' && mkdir -p '" + tmp + "'").c_str()) != 0) return 1;
        if (chdir(tmp.c_str()) != 0) return 1;
        std::string msg;
        const bool ok = hipi::printout_save(*printer, msg);
        const bool ok2 = hipi::printout_save(*printer, msg);
        check(ok && ok2 && msg == "printout_2", "saved twice: printout_1, printout_2");
        FILE* f = std::fopen("printouts/printout_1.bmp", "rb");
        std::vector<unsigned char> bmp;
        if (f) { int c; while ((c = std::fgetc(f)) != EOF) bmp.push_back(static_cast<unsigned char>(c)); std::fclose(f); }
        auto le32 = [&](std::size_t o) { return bmp[o] | bmp[o + 1] << 8 | bmp[o + 2] << 16 | bmp[o + 3] << 24; };
        const std::size_t lines = printer->lines().size();
        check(bmp.size() > 62 && bmp[0] == 'B' && le32(18) == 352 && le32(22) == static_cast<int>(lines * 22 + 16) && bmp[28] == 1,
              "BMP: 352 wide (168 dots x 2 + margins), 22 px a line, 1 bit per pixel");
        long black = 0;
        for (std::size_t i = 62; i < bmp.size(); ++i) black += __builtin_popcount(bmp[i]);
        check(black > 1000, "... with the print in it");
        std::ifstream t("printouts/printout_1.txt");
        std::string all((std::istreambuf_iterator<char>(t)), std::istreambuf_iterator<char>());
        check(all.find("01*LBL \"FIB\"") != std::string::npos && all.find("LINE 39") != std::string::npos &&
              all.find("[graphics]") != std::string::npos, "TXT: the listing, the lines, [graphics] for the sine curve");
        if (chdir(cwd) != 0) return 1;
        if (std::system(("cp '" + tmp + "/printouts/printout_1.bmp' tools/hipiview/printer_view_printout.bmp").c_str()) != 0) ++failures;
    }
    const std::deque<CPrinter::Line> savedPaper = printer->lines();

    // ── Tearing off: up to the edge, then sideways ─────────────────────
    hipi::views_paperDragBegin(620, 300);
    for (int y = 300; y >= 100; y -= 20) { hipi::views_paperDragMove(620, y); run(20); }
    int dir = 0;
    for (int x = 620; x >= 480; x -= 20) { hipi::views_paperDragMove(x, 100); run(20); }
    hipi::views_paperDragEnd();
    check(hipi::views_takeTearGesture(dir) && dir == -1, "up to the edge, then left: tear off (to the left)");
    check(!hipi::views_takeTearGesture(dir), "... asked once");
    // Not at the edge: sideways does nothing
    hipi::views_paperDragBegin(620, 200);
    hipi::views_paperDragMove(620, 230);
    hipi::views_paperDragMove(400, 230);
    hipi::views_paperDragEnd();
    check(!hipi::views_takeTearGesture(dir), "sideways without being at the edge: nothing");
    hipi::views_tearOff(-1);
    const int inkBefore = inkRows(24, 340);
    run(40);
    const int inkMid = inkRows(24, 340);
    check(inkMid > 0 && inkMid < inkBefore, "tearing: the paper slides away to the side");
    snapshot("tools/hipiview/printer_view_7_tearing.png");
    run(1000);
    check(hipi::views_paperLines() == 0 && inkRows(24, 340) == 0, "torn off: the paper is empty");
    hipi::views_paperScrollBy(5);
    check(!hipi::views_paperScrolled(), "torn off: nothing older to show");

    // ── Printouts: listed, loaded back into the printer, deleted ───────
    {
        char cwd[512];
        if (getcwd(cwd, sizeof(cwd)) == nullptr) return 1;
        const std::string tmp = std::string(cwd) + "/tools/hipiview/printout_sd";
        if (chdir(tmp.c_str()) != 0) return 1;
        const auto list = hipi::printout_list();
        check(list.size() == 2 && list[0].number == 2 && list[1].number == 1 &&
              list[0].lines == static_cast<int>(savedPaper.size()), "listed: printout_2, printout_1 (newest first), with their lines");
        std::string msg;
        const bool ok = hipi::printout_load(1, *printer, msg);
        bool same = ok && printer->lines().size() == savedPaper.size();
        for (std::size_t i = 0; same && i < savedPaper.size(); ++i) {
            const auto& a = printer->lines()[i];
            const auto& b = savedPaper[i];
            if (a.dots != b.dots) same = false;
            if (!b.text.empty() && a.text != b.text) same = false;
        }
        check(same, "loaded: the same dots and text as when it was saved");
        hipi::views_reloadPaper();
        run(300);
        snapshot((std::string(cwd) + "/tools/hipiview/printer_view_8_loaded.png").c_str());
        check(inkRows(24, 340) > 1000 && !hipi::views_paperScrolled(), "loaded: on the paper again, at the live position");
        hipi::views_paperScrollBy(5);
        check(hipi::views_paperScrolled(), "... and it can be scrolled back");
        hipi::views_paperToLive();
        run(1000);
        check(hipi::printout_delete(2), "deleted printout_2");
        FILE* f1 = std::fopen("printouts/printout_2.bmp", "rb");
        FILE* f2 = std::fopen("printouts/printout_2.txt", "rb");
        check(f1 == nullptr && f2 == nullptr, "... both the .bmp and the .txt");
        if (f1) std::fclose(f1);
        if (f2) std::fclose(f2);
        check(hipi::printout_list().size() == 1, "... and it's no longer listed");
        check(!hipi::printout_load(7, *printer, msg) && printer->lines().size() == savedPaper.size(),
              "loading one that's not there: an error, the paper as it was");
        if (chdir(cwd) != 0) return 1;
        if (std::system(("rm -rf '" + tmp + "'").c_str()) != 0) ++failures;
    }

    // ── Plotter: DP -- the digitize panel; a touch sets the marker, the
    //    arrows move it a pixel, ENTER gives the point, X cancels ─────────
    hipi::views_setPlotFit(false);                    // Page: the whole paper, known mapping
    for (char c : std::string("IN;SP1;PA1000,1000;PD;PA9000,1000,9000,6500,1000,6500,1000,1000;"
                              "PU;PA2000,2000;PD;CI1500;PU;PA5150,3825;")) plotter->put(c);
    for (char c : std::string("DP;")) plotter->put(c);
    run(100);
    check(hipi::views_output() == hipi::DisplayOutput::Plotter && hipi::views_digitizing(),
          "DP: the Plotter view with the digitize panel");
    int mx = 0, my = 0;
    hipi::views_digitizeMarker(mx, my);
    std::printf("     marker at the pen: %d,%d\n", mx, my);
    check(mx == 367 && my == 300, "... the marker starts at the pen (5150,3825)");
    check(hipi::views_digitizeTap(200, 400) && plotter->digitizing(), "a touch on the plot: the marker moves, no point yet");
    hipi::views_digitizeMarker(mx, my);
    check(mx == 200 && my == 400, "... the marker where the finger was");
    hipi::views_digitizeTap(72, 501);                 // the frame's corner (1000,1000): in the loupe
    snapshot("tools/hipiview/digitize_1_white.png");
    hipi::views_digitizeTap(200, 400);
    int bx = 0, by = 0;
    hipi::views_digitizeControl(3, bx, by);           // right
    for (int i = 0; i < 3; ++i) { hipi::views_digitizeTap(bx, by); hipi::views_digitizeRelease(); }
    hipi::views_digitizeControl(0, bx, by);           // up
    hipi::views_digitizeTap(bx, by); hipi::views_digitizeRelease();
    hipi::views_digitizeMarker(mx, my);
    check(mx == 203 && my == 399, "arrows: 3 right, 1 up -- a pixel each");
    hipi::views_digitizeTap(bx, by);                  // up, held: repeats after a moment
    run(1000);
    hipi::views_digitizeRelease();
    hipi::views_digitizeMarker(mx, my);
    std::printf("     up held 1 s: y %d\n", my);
    check(my < 399 - 5, "an arrow held: repeats");
    hipi::views_digitizeTap(200, 400);
    hipi::views_digitizeButton(3, false);             // the strip's Up
    hipi::views_digitizeMarker(mx, my);
    check(mx == 200 && my == 399, "the strip's Up moves it too");
    // Page in the narrowed area: 734 px wide (1024 - strip 50 - panel 240)
    const double sc = std::min(734.0 / 10300, 600.0 / 7650), dy = (600 - 7650 * sc) / 2;
    const int wantX = static_cast<int>(std::lround(200 / sc)), wantY = static_cast<int>(std::lround((dy + 7650 * sc - 399) / sc));
    hipi::views_digitizeControl(4, bx, by);           // ENTER
    hipi::views_digitizeTap(bx, by);
    std::printf("     ENTER: %d,%d (expected %d,%d)\n", plotter->penX(), plotter->penY(), wantX, wantY);
    check(!plotter->digitizing() && std::abs(plotter->penX() - wantX) <= 2 && std::abs(plotter->penY() - wantY) <= 2,
          "ENTER: the point under the marker, given to the plotter");
    check(!hipi::views_digitizing(), "... and the panel gone");
    run(1500);
    hipi::views_setPaperBlack(true);
    for (char c : std::string("DP;")) plotter->put(c);
    run(100);
    hipi::views_digitizeTap(185, 327);                // on the circle
    snapshot("tools/hipiview/digitize_2_black.png");
    hipi::views_digitizeControl(5, bx, by);           // X
    hipi::views_digitizeTap(bx, by);
    check(!plotter->digitizing() && !hipi::views_digitizing(), "X: cancelled, the panel gone");
    hipi::views_setPaperBlack(false);
    check(!hipi::views_digitizeTap(100, 100), "no DP: a touch isn't a point");
    // DP right after IN: the pen at 0,0 -- on the Page view that's the
    // paper's corner: the marker there, its arms cut at the screen's edge
    // (given to the chip as negative / too large corners they drew magenta
    // lines along the left and bottom edges on the real panel; the emulator
    // clips them anyway, so this only shows the case)
    for (char c : std::string("IN;DP;")) plotter->put(c);
    run(100);
    hipi::views_digitizeMarker(mx, my);
    chip.render(panel.data(), 0.0);
    long stray = 0;
    for (int y = 0; y < SCREEN_MAX_Y; ++y)
        for (int x = 0; x < 734; ++x) {
            if (std::abs(x - mx) <= 20 && std::abs(y - my) <= 20) continue;   // (the marker itself)
            const std::uint32_t p = at(x, y);
            if (((p >> 16) & 0xFF) > 200 && (p & 0xFF) > 200 && ((p >> 8) & 0xFF) < 60) ++stray;
        }
    std::printf("     marker %d,%d -- stray magenta pixels: %ld\n", mx, my, stray);
    snapshot("tools/hipiview/digitize_3_corner.png");
    check(stray == 0, "DP at the pen 0,0: no magenta outside the marker");
    hipi::views_digitizeControl(5, bx, by);
    hipi::views_digitizeTap(bx, by);

    // ── Plotter: the pen's tip -- only what's drawn after the change gets
    //    it: a thin and a wide line in one plot, kept through a redraw ──
    check(!hipi::views_penWide(), "Pen: thin at start");
    for (char c : std::string("IN;")) plotter->put(c);
    run(100);
    chip.render(panel.data(), 0.0);
    std::vector<std::uint32_t> blankCol(SCREEN_MAX_Y);
    for (int y = 0; y < SCREEN_MAX_Y; ++y) blankCol[static_cast<std::size_t>(y)] = at(500, y);
    // Rows changed in column 500 between two plotter y's (screen rows around each line)
    auto rowsNear = [&](int yFrom, int yTo) {
        chip.render(panel.data(), 0.0);
        int n = 0;
        for (int y = yFrom; y < yTo; ++y) if (at(500, y) != blankCol[static_cast<std::size_t>(y)]) ++n;
        return n;
    };
    for (char c : std::string("SP1;PU;PA1000,5000;PD;PA9000,5000;PU;")) plotter->put(c);   // thin (upper)
    hipi::views_setPenWide(true);
    for (char c : std::string("PA1000,3000;PD;PA9000,3000;PU;")) plotter->put(c);          // wide (lower)
    hipi::views_setPenWide(false);
    for (char c : std::string("SP2;PA2000,2000;PD;CI1500;PU;")) plotter->put(c);           // thin again
    hipi::views_setPenWide(true);
    for (char c : std::string("PA4000,1500;SI0.4,0.6;LBWIDE PEN\x03;PU;")) plotter->put(c);
    hipi::views_setPenWide(false);
    run(100);
    const int thinRows = rowsNear(0, 300), wideRows = rowsNear(300, 400);
    std::printf("     column 500: %d row(s) for the thin line, %d for the wide one\n", thinRows, wideRows);
    check(thinRows == 1 && wideRows == 3, "Pen: the thin line one pixel across, the wide one three");
    hipi::views_redraw();
    check(rowsNear(0, 300) == 1 && rowsNear(300, 400) == 3, "Pen: each keeps its own width when the plot is redrawn");
    snapshot("tools/hipiview/printer_view_wide_pen.png");

    // ── Plotter: Fit after IN -- a new, smaller plot inside the old one's
    //    area is fitted afresh, not drawn at the old scale ───────────────
    hipi::views_setPlotFit(true);
    for (char c : std::string("IN;SP1;PU;PA0,0;PD;PA10000,0,10000,7000,0,7000,0,0;PU;")) plotter->put(c);
    run(100);
    for (char c : std::string("IN;SP1;PU;PA3000,2000;PD;PA6000,2000,6000,4000,3000,4000,3000,2000;PU;")) plotter->put(c);
    run(100);
    {
        int ax = 0, ay = 0, bx = 0, by = 0;
        hipi::views_plotToScreen(3000, 2000, ax, ay);
        hipi::views_plotToScreen(6000, 4000, bx, by);
        std::printf("     the small plot after IN: %d x %d pixels\n", bx - ax, ay - by);
        check(bx - ax > 600, "Fit after IN: the new plot fills the screen (not the old plot's scale)");
    }
    hipi::views_setPlotFit(false);

    // ── Plotter: the ERROR light -- lit by an HP-GL error, on top of the
    //    plot, a touch says what it was, OE puts it out ───────────────────
    {
        for (char c : std::string("IN;SP1;PU;PA1000,1000;PD;PA9000,7000;PU;")) plotter->put(c);
        run(100);
        int lx = 0, ly = 0;                  // the LED: top centre, 18 px into the box
        lx = ((1024 - 112) / 2 & ~3) + 18;
        ly = 8 + 16;
        chip.render(panel.data(), 0.0);
        const std::uint32_t paper = at(lx, ly);
        check(((paper >> 16) & 0xFF) < 200 || (paper & 0xFF) > 60, "no error: no ERROR light");
        for (char c : std::string("SC0,100;")) plotter->put(c);   // error 2
        run(50);
        chip.render(panel.data(), 0.0);
        const std::uint32_t led = at(lx, ly);
        std::printf("     the LED: %06X\n", led);
        check(((led >> 16) & 0xFF) > 150 && ((led >> 8) & 0xFF) > 150 && (led & 0xFF) < 60, "SC with two numbers: the ERROR light is lit (yellow)");
        snapshot("tools/hipiview/printer_view_error_light.png");
        // A line drawn right through it: the light stays on top
        for (char c : std::string("PU;PA0,7600;PD;PA10300,7600;PU;")) plotter->put(c);
        run(50);
        chip.render(panel.data(), 0.0);
        check(at(lx, ly) == led, "a line drawn through it: the light stays on top");
        check(hipi::views_errorText() == "Error 2: wrong number of parameters -- SC 0,100",
              "its text: \"Error 2: wrong number of parameters -- SC 0,100\"");
        check(!hipi::views_errorLightTap(lx, 300), "a touch beside it: not taken");
        check(hipi::views_errorLightTap(lx, ly) && hipi::views_isSplashVisible(),
              "a touch on it: the message box");
        run(3000);
        for (char c : std::string("OE;")) plotter->put(c);         // reading the error puts it out
        run(50);
        chip.render(panel.data(), 0.0);
        check(at(lx, ly) != led && plotter->errorCode() == 0, "OE: the light goes out, the plot under it back");
        check(!hipi::views_errorLightTap(lx, ly), "... and a touch there does nothing");
        for (char c : std::string("ZZ;")) plotter->put(c);
        run(50);
        hipi::views_redraw();
        chip.render(panel.data(), 0.0);
        check(at(lx, ly) == led, "an error, then the plot redrawn: the light is drawn again");
        plotter->clearError();
        run(50);
        chip.render(panel.data(), 0.0);
        check(at(lx, ly) != led, "Clear error (the menu): out");
    }

    // A message box ("Saved screendump_3.bmp" ...) writes its text with the
    // chip's one text cursor: afterwards it's back at the display's cursor
    hipi::views_showView(hipi::DisplayOutput::Display, nullptr);
    run(2000);
    for (char c : std::string("01 LBL \"A\"\r\n02 X")) screen.pr_char(static_cast<std::uint8_t>(c));
    auto textCursor = [] {
        return std::vector<std::uint8_t>{ chip.reg(0x63), chip.reg(0x64), chip.reg(0x65), chip.reg(0x66) };
    };
    const std::vector<std::uint8_t> cur = textCursor();
    hipi::views_showMessage("Saved screenshots/screendump_3.bmp");
    check(textCursor() == cur, "message box: the text cursor is put back where the display has it");
    run(3000);
    check(textCursor() == cur, "... and is still there after the box has gone");

    // The view HIPI starts with: the first device on the loop that is
    // switched on and has a view -- not TFDISPLAY if it's switched off
    {
        struct ViewDev : CDevice {
            ViewKind k;
            ViewDev(const char* n, ViewKind kind) : CDevice(n, 0, 0, DISPLAY), k(kind) {}
            ViewKind viewKind() const override { return k; }
            void clear() override {}
        } disp("TFDISPLAY", ViewKind::Display), leds("TFLEDS", ViewKind::None);
        const std::vector<CDevice*> saved = devices;
        const bool printerOn = printer->enabled(), plotterOn = plotter->enabled();
        devices = { &leds, &disp, plotter, printer };
        disp.setEnabled(true); plotter->setEnabled(true); printer->setEnabled(true);
        hipi::views_showStartView();
        check(hipi::views_output() == hipi::DisplayOutput::Display && hipi::views_viewDevice() == &disp,
              "start view: TFDISPLAY, the first device with a view (TFLEDS has none)");
        disp.setEnabled(false);
        hipi::views_showStartView();
        check(hipi::views_output() == hipi::DisplayOutput::Plotter && hipi::views_viewDevice() == plotter,
              "start view, TFDISPLAY switched off: the next one, TFPLOT");
        devices = { &leds, &disp, printer, plotter };
        hipi::views_showStartView();
        check(hipi::views_viewDevice() == printer, "... in loop order: TFPRINT before TFPLOT");
        plotter->setEnabled(false); printer->setEnabled(false);
        hipi::views_showStartView();
        check(hipi::views_output() == hipi::DisplayOutput::LoopMap,
              "start view, no device switched on has a view (the printer off): the loop map");
        // Leaving the loop map, the view before it gone: the first one again
        plotter->setEnabled(true);
        hipi::views_leaveLoopMap();
        check(hipi::views_output() == hipi::DisplayOutput::Plotter, "leaving the loop map: the first view there is");
        hipi::views_showStartView();
        run(3000);
        devices = saved;
        printer->setEnabled(printerOn); plotter->setEnabled(plotterOn);
    }

    std::printf("%s\n", failures ? "FAILED" : "ALL PASSED");
    return failures ? 1 : 0;
}
