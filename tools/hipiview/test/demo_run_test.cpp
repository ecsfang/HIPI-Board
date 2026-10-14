// demo_run_test.cpp -- the demo (System -> Demo) run on the PC: the real
// demo.cpp with its script, the real views, Screen, Video80, plotter and
// printer against the emulated chip; stand-ins for the SD card (a folder
// with resources/ and lif/), the analyzer, the LEDs/pixels and the loop.
// A whole round is run in fake time, a picture saved a moment after each
// view change (demo_run_*.png), then it's stopped by HP-IL traffic and
// checked that everything is put back.
//
//   make demo_run_test && ./demo_run_test   (in tools/hipiview)
#include "LT7683.hpp"
#include "Screen.hpp"
#include "plotterview.h"
#include "printer.h"
#include "plotter.h"
#include "drive.h"
#include "display.h"
#include "video80.h"
#include "demo.h"
#include "hwtest.h"
#include "hwio_stub.h"
#include "analyzer.h"
#include "loopmap.h"
#include "config.hpp"
#include "clock.h"
#include "hpil_diag.h"
#include "mediamap.h"
#include "hp82163_font.hpp"
#include "../lt7683_emu.hpp"
#include "../png.hpp"

#include <algorithm>
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
CDisplay* videoDisplay = nullptr;
hipi::Screen* screen = nullptr;
hipi::Video80* video80Term = nullptr;
hipi::Config config;

// The loop for the demo's made-up session: HIPI's devices only
unsigned short hipi_simFrame(unsigned short f) {
    const IL_CMD_t in = f;
    for (CDevice* d : devices) if (d->enabled()) f = d->hpil(f);
    hipi::analyzer_capture(in, f);
    return f;
}
void hipi_bootService() {}
void hipi_bootServiceDone() {}

// The mirror isn't used here -- except how far hipiview is (the demo
// waits for it): set by the test
bool mirrorSyncing = false;
std::uint32_t mirrorBacklog = 0;
namespace hipi {
bool displayMirror_syncing() { return mirrorSyncing; }
std::uint32_t displayMirror_backlog() { return mirrorBacklog; }
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
        hipi::hwtest_poll();
        hipi::demo_poll();
        if (videoDisplay) videoDisplay->idle();
    }
}



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

const char* outName(hipi::DisplayOutput o) {
    using O = hipi::DisplayOutput;
    switch (o) {
        case O::Display: return "display"; case O::Video80: return "video80"; case O::Tape: return "tape";
        case O::Plotter: return "plotter"; case O::Printer: return "printer"; case O::Analyzer: return "analyzer";
        case O::LoopMap: return "loopmap"; case O::Signals: return "signals"; case O::Clock: return "saver";
        case O::MediaMap: return "mediamap";
        default: return "other";
    }
}

}  // namespace

int main(int argc, char** argv) {
    // The "SD card": a folder with the pictures and the cassettes
    const std::string root = argc > 1 ? argv[1] : "../..";
    char cwd[512];
    if (getcwd(cwd, sizeof(cwd)) == nullptr) return 1;
    const std::string here = cwd;
    const std::string sd = here + "/demo_sd";
    std::string cmd = "rm -rf '" + sd + "' && mkdir -p '" + sd + "' && ln -s '" + here + "/" + root + "/resources' '" + sd +
                      "/resources' && ln -s '" + here + "/" + root + "/sdcard/lif' '" + sd + "/lif'";
    if (std::system(cmd.c_str()) != 0 || chdir(sd.c_str()) != 0) { std::perror("demo_sd"); return 1; }

    ChipTransport t;
    hipi::LT7683 d(t, SCREEN_MAX_X, SCREEN_MAX_Y);
    d.begin();
    makeChipFont();
    screen = new hipi::Screen(&d, 0x07E0, 1, 200, 1024);
    video80Term = new hipi::Video80(&d);
    plotter = new CPlotter("TFPLOT");
    printer = new CPrinter("TFPRINT");
    CDrive* drive = new CDrive();
    videoDisplay = new CDisplay("TFDISPLAY");
    videoDisplay->setVideo80(video80Term);
    videoDisplay->setEnabled(true);
    drive->setEnabled(true);
    plotter->setEnabled(true);
    devices = { videoDisplay, drive, plotter, printer };
    hipi::plotterview_setPrinter(printer);
    hipi::plotterview_setDrive(drive);
    hipi::plotterview_init(&d, screen, plotter);
    hipi::plotterview_setVideo80(video80Term);
    hipi::hwtest_init(&d);
    hipi::hpil_diag_init(&d);
    int pickerShows = 0, pickerMoves = 0, pickerHides = 0;
    std::vector<std::string> pickerFiles;
    hipi::DemoHooks h;
    h.showPicker = [&](const std::vector<std::string>& f, int) { ++pickerShows; pickerFiles = f; };
    h.movePicker = [&](int) { ++pickerMoves; };
    h.hidePicker = [&] { ++pickerHides; };
    hipi::demo_init(&d, h, "1.2.3");
    run(2000);

    // Something on the screen and the plot before: gone afterwards
    for (char c : std::string("BEFORE THE DEMO\r\n")) screen->pr_char(static_cast<std::uint8_t>(c));
    for (char c : std::string("IN;SP1;PU0,0;PD1000,1000;")) plotter->put(c);
    const IL_CMD_t addrBefore = plotter->addr();

    hipi::demo_start();
    check(hipi::demo_running(), "System -> Demo: it runs");
    run(50);
    check(hipi::plotterview_output() == hipi::DisplayOutput::Display, "it starts on the Display view");

    // hipiview connected and still getting the whole screen: the demo waits
    run(1500);
    std::vector<std::uint32_t> before(1024 * 600), after(1024 * 600);
    chip.render(before.data(), 0.0);
    mirrorSyncing = true;
    run(5000);
    chip.render(after.data(), 0.0);
    check(before == after, "hipiview still syncing: the demo stands still (5 s)");
    mirrorSyncing = false;
    run(1000);
    chip.render(after.data(), 0.0);
    check(before != after, "... and goes on once it's in step");
    // A viewer that never catches up: a step waits 3 s at most, then on
    mirrorBacklog = 100000;
    {
        const hipi::DisplayOutput o0 = hipi::plotterview_output();
        bool moved = false;
        int k_ = 0;
        for (k_ = 0; k_ < 150 && !moved; ++k_) { run(1000); moved = hipi::plotterview_output() != o0; }
        std::printf("     (next view after %d s)\n", k_);
        check(moved, "hipiview never catching up: the demo still goes on");
    }
    mirrorBacklog = 0;
    hipi::demo_stop();
    hipi::clockStubAvailable() = true;          // the RTC is there and set: the clock gets a turn
    hipi::demo_start();
    run(50);

    // A whole round: a picture shortly after each new view
    std::vector<std::string> seen;
    hipi::DisplayOutput last = hipi::plotterview_output();
    int shots = 0;
    bool plotted = false, printed = false, analyzed = false, wentBack = false;
    bool mapRings = false, signalsExample = false, scrolled = false, tornOff = false, clockShown = false;
    int maxLines = 0;
    std::uint64_t changedAt = fakeNowUs;
    bool shotDone = false;
    for (int ms = 0; ms < 260000; ms += 5) {
        run(5);
        const hipi::DisplayOutput o = hipi::plotterview_output();
        if (o != last) {
            // (the clock's own turn first, then the screen saver at the end)
            if (last == hipi::DisplayOutput::Clock && o != hipi::DisplayOutput::Clock &&
                hipi::clockStubShow() != hipi::SaverShow::Clock) wentBack = true;
            if (o == hipi::DisplayOutput::Clock && hipi::clockStubShow() == hipi::SaverShow::Clock) clockShown = true;
            last = o;
            seen.push_back(outName(o));
            changedAt = fakeNowUs;
            shotDone = false;
        }
        if (!shotDone && fakeNowUs - changedAt > 2500000) {
            char png[64];
            std::snprintf(png, sizeof(png), "../demo_run_%02d_%s.png", ++shots, outName(o));
            snapshot(png);
            shotDone = true;
        }
        if (!plotter->segments().empty()) plotted = true;
        if (o == hipi::DisplayOutput::MediaMap && hipi::mediamap_rings() > 0) mapRings = true;
        if (o == hipi::DisplayOutput::Printer && hipi::plotterview_paperScrolled() && !scrolled) {   // (pulled up)
            scrolled = true;
            run(1500);
            snapshot("../demo_run_printer_pulled_up.png");
        }
        maxLines = std::max(maxLines, hipi::plotterview_paperLines());
        if (o == hipi::DisplayOutput::Printer && maxLines > 0 && hipi::plotterview_paperLines() == 0) tornOff = true;
        if (o == hipi::DisplayOutput::Signals && shotDone && !signalsExample) {
            chip.render(panel.data(), 0.0);
            // The cyan "EXAMPLE" band under the title, and a waveform (yellow) in it
            const std::uint32_t title = at(1010, 10), example = at(1010, 36);
            int band = 0, trace = 0;
            for (int x = 0; x < 1024; ++x) if (at(x, 36) == example) ++band;
            if (example == 0 || example == title) band = 0;
            for (int y = 110; y < 230; ++y) for (int x = 20; x < 1004; ++x) if (at(x, y) == title) ++trace;
            std::printf("     signals: band %d px, trace %d px\n", band, trace);
            signalsExample = band > 500 && trace > 500;
        }
        if (printer->lineCount() > 0) printed = true;
        if (hipi::analyzerStubFrames() > 20) analyzed = true;
        if (wentBack) break;
    }
    std::string order;
    for (const std::string& s : seen) order += s + " ";
    std::printf("     views: %s\n", order.c_str());
    check(order.find("video80") != std::string::npos, "the display as an HP 92198");
    check(order.find("tape") != std::string::npos && pickerShows == 1 && pickerMoves >= 1 && pickerHides == 1,
          "Tape: the cassette list shown, a cassette picked");
    check(!pickerFiles.empty() && pickerFiles[0].empty() && pickerFiles.size() >= 2, "... from the lif folder");
    check(plotted, "the plotter draws");
    check(printed, "the printer prints");
    check(analyzed && order.find("loopmap") != std::string::npos && order.find("signals") != std::string::npos,
          "the analyzer (a made-up session), the loop map, the signals");
    check(order.find("mediamap") != std::string::npos && mapRings, "the media map of the picked cassette");
    check(scrolled && tornOff, "the paper: pulled up to the edge, then torn off");
    check(signalsExample, "HP-IL signals without traffic: the example, not an empty page");
    check(clockShown, "the RTC is set: the clock gets its own turn");
    check(order.find("saver") != std::string::npos && wentBack, "a screen saver, then round again");
    check(hipi::demo_running(), "still running");

    // Real HP-IL traffic stops it
    run(3000);
    hipi::demo_hpilActivity();
    run(10);
    check(!hipi::demo_running(), "HP-IL traffic stops it");
    check(hipi::plotterview_output() == hipi::DisplayOutput::Display, "back to the view it started from");
    check(plotter->segments().empty() && printer->lines().empty(), "the plot and the paper are empty");
    check(plotter->addr() == addrBefore, "the devices' loop addresses put back");
    check(drive->mediaFile().empty(), "the cassette wasn't really changed");
    snapshot("../demo_run_99_after.png");

    // A touch stops it too (boardui calls demo_stop())
    hipi::demo_start();
    run(4000);
    hipi::demo_stop();
    check(!hipi::demo_running() && hipi::plotterview_output() == hipi::DisplayOutput::Display, "stopped: back");
    chdir(here.c_str());
    std::system(("rm -rf '" + sd + "'").c_str());
    std::printf("%s\n", failures ? "FAILED" : "ALL PASSED");
    return failures ? 1 : 0;
}
