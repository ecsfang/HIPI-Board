// printer_paper_test.cpp -- the printer's paper in the display chip's SDRAM
// (printer_paper.h): the real CPrinter, printer_paper.cpp and the LT7683
// driver against the emulated chip. 8192 lines kept instead of 200; the
// oldest go when it's full; tearing off; a printout of thousands of lines
// saved and loaded back a line at a time (printout.cpp).
//
//   make printer_paper_test && ./printer_paper_test   (in tools/hipiview)
#include "LT7683.hpp"
#include "printer.h"
#include "printer_paper.h"
#include "printout.h"
#include "../lt7683_emu.hpp"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>

bool bTrace = false, bExtTrace = false;
std::uint64_t fakeNowUs = 1000000;
namespace hipi {
int quietDepth = 0, quietCalls = 0;
void displayMirror_quiet(bool q) { if (q) { ++quietDepth; ++quietCalls; } else --quietDepth; }
}

namespace {
LT7683Emu chip;
std::size_t spiBytes = 0;
class ChipTransport : public hipi::DisplayTransport {
public:
    void csLow() override { first_ = true; }
    void csHigh() override {}
    void rstLow() override {}
    void rstHigh() override {}
    void spiTransfer(const std::uint8_t* tx, std::uint8_t* rx, std::size_t len) override {
        spiBytes += len;
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
void print(CPrinter& p, const std::string& s) {
    for (char c : s) p.put(static_cast<std::uint8_t>(c));
    p.put('\r');
}
std::string text(const CPrinter& p, std::uint32_t n) {
    const CPrinter::Line* l = p.line(n);
    if (l == nullptr) return "(none)";
    std::string t = l->text;
    while (!t.empty() && t.back() == ' ') t.pop_back();
    return t;
}
}  // namespace

int main() {
    ChipTransport t;
    hipi::LT7683 d(t, SCREEN_MAX_X, SCREEN_MAX_Y);
    d.begin();
    {
        // Without a store: 200 lines in RAM, as before
        CPrinter p("TFPRINT");
        for (int i = 0; i < 300; ++i) print(p, "LINE " + std::to_string(i));
        check(p.paperCapacity() == 200 && p.keptLines() == 200 && p.firstKept() == 100 && text(p, 100) == "LINE 100",
              "no store: the last 200 lines kept in RAM");
    }
    CPrinter p("TFPRINT");
    hipi::printerPaper_init(&d, &p);
    check(p.paperCapacity() == 8192 && p.keptLines() == 0, "in SDRAM: room for 8192 lines");
    // 9000 lines: the last 8192 kept, the first ones gone
    for (int i = 0; i < 9000; ++i) print(p, "LINE " + std::to_string(i));
    std::printf("     %u lines printed, %u kept from line %u\n", static_cast<unsigned>(p.lineCount()),
                static_cast<unsigned>(p.keptLines()), static_cast<unsigned>(p.firstKept()));
    check(p.lineCount() == 9000 && p.keptLines() == 8192 && p.firstKept() == 808, "9000 printed: the last 8192 kept");
    check(text(p, 808) == "LINE 808" && text(p, 4000) == "LINE 4000" && text(p, 8999) == "LINE 8999" && p.line(807) == nullptr,
          "... each one read back as it was printed; the older ones gone");
    const CPrinter::Line* l = p.line(8999);
    int dots = 0;
    for (std::uint8_t c : l->dots) dots += c != 0;
    check(dots > 20, "... with its dots");
    check(hipi::quietDepth == 0 && hipi::quietCalls > 9000, "the SDRAM traffic: kept from a viewer (displayMirror_quiet)");
    // The view reads a line once (the same line again: from the buffer)
    std::size_t before = spiBytes;
    p.line(5000);
    const std::size_t one = spiBytes - before;
    before = spiBytes;
    p.line(5000);
    check(one > 200 && spiBytes == before, "a line: one read over SPI; again: none");
    // Torn off: empty, lineCount() goes on
    p.clearPaper();
    check(p.keptLines() == 0 && p.line(8999) == nullptr && p.lineCount() == 9000, "torn off: the paper empty");

    // A printout of 3000 lines (more than the old 200): saved and loaded
    // back a line at a time
    for (int i = 0; i < 3000; ++i) print(p, "ROW " + std::to_string(i));
    char cwd[512];
    if (getcwd(cwd, sizeof(cwd)) == nullptr) return 1;
    const std::string tmp = std::string(cwd) + "/paper_sd";
    if (std::system(("rm -rf '" + tmp + "' && mkdir -p '" + tmp + "'").c_str()) != 0) return 1;
    if (chdir(tmp.c_str()) != 0) return 1;
    std::string msg;
    CPrinter::Line last;
    p.readLine(p.lineCount() - 1, last);
    check(hipi::printout_save(p, msg) && msg == "printout_1", "3000 lines saved: printout_1");
    p.clearPaper();
    check(hipi::printout_load(1, p, msg) && p.keptLines() == 3000, "... loaded back: 3000 lines on the paper");
    check(text(p, p.firstKept()) == "ROW 0" && text(p, p.firstKept() + 1234) == "ROW 1234" &&
          text(p, p.lineCount() - 1) == "ROW 2999", "... the same text");
    CPrinter::Line a;
    p.readLine(p.lineCount() - 1, a);
    check(a.dots == last.dots, "... and the same dots");
    if (chdir(cwd) != 0) return 1;
    if (std::system(("rm -rf '" + tmp + "'").c_str()) != 0) ++failures;

    std::printf("%s\n", failures ? "FAILED" : "ALL PASSED");
    return failures ? 1 : 0;
}
