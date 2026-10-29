// loopmap_test.cpp -- the HP-IL loop map on the PC: the real loopmap.cpp
// with the LT7683 driver against the emulated chip, fed made-up HP-41
// traffic through a stand-in for the analyzer's capture (test/lmstubs).
// Checks that talker and listener are marked on their cards (and on the
// controller) and stay marked for a moment after they're unaddressed.
// Saves:
//
//   loopmap_1_listener.png   the HP-41 sends to the printer (LAD 02)
//   loopmap_2_talker.png     the HP-41 asks the display who it is (TAD 01, SAI)
//   loopmap_3_both.png       the plotter is talker and listener at once
//   loopmap_4_details.png    TFPLOT's details box, with GO TO PLOTTER VIEW
//   loopmap_5_no_view.png    a device after HIPI: no view of its own
//   loopmap_5b_error.png     TFPLOT's details box with an HP-GL error
//
//   make loopmap_test && ./loopmap_test   (in tools/hipiview)
#include "LT7683.hpp"
#include "loopmap.h"
#include "page_chrome.h"
#include "demo_content.h"
#include "analyzer.h"
#include "views.h"
#include "plotter.h"
#include "hp82163_font.hpp"
#include "../lt7683_emu.hpp"
#include "../png.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

std::uint64_t fakeNowUs = 1000000;
bool bTrace = false, bExtTrace = false;   // (hpgl.cpp logs under trace)
std::vector<CDevice*> devices;

// The views: the loop map is shown; a device's view is "opened" here only
// to see that the details box asks for it
CDevice* viewOpened = nullptr;
namespace hipi {
DisplayOutput views_output() { return DisplayOutput::LoopMap; }
bool views_isSplashVisible() { return false; }
const char* views_viewName(CDevice* d) {
    switch (d->type()) {
        case PLOTTER: return "PLOTTER";
        case PRINTER: return "PRINTER";
        case DISPLAY: return "DISPLAY";
        default:      return nullptr;
    }
}
void views_showDeviceFromLoopMap(CDevice* d) { viewOpened = d; }
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

class Dev : public CDevice {
public:
    Dev(const char* n, IL_ADDR_t aid, IL_Type_e t, int a) : CDevice(n, aid, 0, t) { addr(static_cast<IL_CMD_t>(a)); }
    void clear() override {}
};

int failures = 0;
std::vector<std::uint32_t> panel(1024 * 600);

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}
void makeChipFont() {
    std::uint8_t f[256][16];
    for (int c = 0; c < 256; ++c) {
        if (c >= 32 && c < 127) std::memcpy(f[c], hipi::font[c - 32], 16);
        else for (int r = 0; r < 16; ++r) f[c][r] = (r == 2 || r == 13) ? 0x7E : (r > 2 && r < 13 ? 0x42 : 0);
    }
    chip.setBuiltinFont(f);
}
void snapshot(const char* png) {
    chip.render(panel.data(), 0.0);
    writePng(png, panel.data(), 1024, 600);
    std::printf("     saved %s\n", png);
}
std::uint32_t at(int x, int y) { return panel[static_cast<std::size_t>(y) * 1024 + x] & 0xFFFFFF; }

// HIPI's devices (as hipi_simFrame() runs them), then the PC's behind PILBOX
unsigned short hipiRound(unsigned short f, unsigned short (*pc)(unsigned short)) {
    const IL_CMD_t in = f;
    for (CDevice* d : devices) f = d->hpil(f);
    if (pc != nullptr) f = pc(f);
    hipi::analyzer_capture(in, f);
    fakeNowUs += 1000;
    return f;
}

void run(int ms) {
    for (int t = 0; t < ms; t += 10) { fakeNowUs += 10000; hipi::loopmap_poll(); }
}
// A frame on the loop (passed on unchanged unless `out` says otherwise), 1 ms apart
void frame(IL_CMD_t in, int out = -1) {
    hipi::analyzer_capture(in, out < 0 ? in : static_cast<IL_CMD_t>(out));
    fakeNowUs += 1000;
}

// Pixels in the talker's / listener's / both's colour above the legend
// (taken from the legend's own frames)
struct Count { int talk, listen, both; };
Count roles() {
    chip.render(panel.data(), 0.0, false);
    const std::uint32_t tc = at(330, 574), lc = at(422, 574), bc = at(530, 574);
    Count c{ 0, 0, 0 };
    for (int y = 30; y < 560; ++y)
        for (int x = 0; x < 1024; ++x) {
            const std::uint32_t p = at(x, y);
            if (p == tc) ++c.talk;
            if (p == lc) ++c.listen;
            if (p == bc) ++c.both;
        }
    return c;
}
// ... in a region (a card or the controller's box)
int inBox(int x0, int y0, int x1, int y1, std::uint32_t col) {
    int n = 0;
    for (int y = y0; y < y1; ++y) for (int x = x0; x < x1; ++x) if (at(x, y) == col) ++n;
    return n;
}

}  // namespace

int main() {
    ChipTransport t;
    hipi::LT7683 d(t, SCREEN_MAX_X, SCREEN_MAX_Y);
    d.begin();
    makeChipFont();
    Dev disp("TFDISPLAY", 0x30, DISPLAY, 1), prt("TFPRINT", 0x20, PRINTER, 2);
    // The plotter is a real CPlotter: loopmap.cpp asks it for its error
    // (static_cast<CPlotter*>), so a stand-in CDevice would be read as one
    CPlotter plot("TFPLOT");
    plot.addr(3);
    devices = { &disp, &prt, &plot };
    hipi::loopmap_init(&d);

    // The HP-41 sets up the loop: HIPI's three devices, 1..3
    frame(AAU);
    frame(AAD + 1, AAD + 4);
    run(600);
    hipi::loopmap_drawAll();
    Count c = roles();
    std::printf("     set up: talker %d px, listener %d px\n", c.talk, c.listen);
    check(c.talk == 0 && c.listen == 0 && c.both == 0, "nobody addressed: no talker, no listener");
    chip.render(panel.data(), 0.0, false);
    const std::uint32_t tc = at(330, 574), lc = at(422, 574), bc = at(530, 574);
    check(tc != lc && tc != bc && lc != bc && tc != 0 && lc != 0 && bc != 0,
          "the legend: talker, listener and both, each its colour");

    // PRA "AB": the printer listens, the HP-41 talks
    frame(UNL);
    frame(LAD + 2);
    frame('A');
    frame('B');
    run(600);
    c = roles();
    snapshot("loopmap_1_listener.png");
    chip.render(panel.data(), 0.0, false);
    // The controller's box is at the left, x 14..130
    const int ctrlTalk = inBox(10, 100, 134, 500, tc);
    std::printf("     printer listens: talker %d px (controller %d), listener %d px\n", c.talk, ctrlTalk, c.listen);
    check(c.listen > 300, "LAD 02: the printer's card is marked as listener");
    check(ctrlTalk > 300 && ctrlTalk == c.talk, "... and the controller as the talker");
    // Unaddressed: still shown for a moment, then gone
    // (the timing follows the hold, kLoopMapRoleHoldMs, whatever it's set to)
    const int hold = static_cast<int>(hipi::kLoopMapRoleHoldMs);
    frame(UNL);
    run(std::max(hold / 2, 20));
    c = roles();
    check(c.listen > 300, "UNL: still shown for a moment (the hold)");
    run(hold + 600);
    c = roles();
    check(c.listen == 0 && c.talk == 0, "... then gone");

    // The HP-41 asks the display who it is: TAD 01, SAI -> 0x30 back
    frame(TAD + 1);
    frame(SAI, 0x30);
    frame(0x30, ETO);
    run(600);
    c = roles();
    chip.render(panel.data(), 0.0, false);
    const int ctrlListen = inBox(10, 100, 134, 500, lc);
    snapshot("loopmap_2_talker.png");
    std::printf("     display talks: talker %d px, listener %d px (controller %d)\n", c.talk, c.listen, ctrlListen);
    check(c.talk > 300 && ctrlListen > 300, "TAD 01: the display is the talker, the controller listens");
    frame(UNT);
    run(hold + 600);
    c = roles();
    check(c.talk == 0 && c.listen == 0, "UNT: gone after the hold");

    // Talker and listener at once (TAD 03 and LAD 03): the third colour
    frame(TAD + 3);
    frame(LAD + 3);
    run(600);
    c = roles();
    snapshot("loopmap_3_both.png");
    std::printf("     plotter both: talker %d px, listener %d px, both %d px\n", c.talk, c.listen, c.both);
    chip.render(panel.data(), 0.0, false);
    check(c.both > 300 && c.talk == 0 && inBox(140, 30, 1024, 560, lc) == 0,
          "TAD 03 + LAD 03: the plotter in the colour for both (the controller listens)");
    frame(UNT);
    run(std::max(hold / 2, 20));
    c = roles();
    check(c.both > 300, "UNT: still both during the hold (it was talker a moment ago)");
    run(hold + 600);
    c = roles();
    chip.render(panel.data(), 0.0, false);
    check(inBox(140, 30, 1024, 560, lc) > 300 && c.both == 0, "... then listener only (still addressed)");

    // ── A page: the X at the right of the title bar closes it ──────────
    hipi::loopmap_drawAll();
    chip.render(panel.data(), 0.0, false);
    {
        // The X: white strokes in a black box on the yellow bar; the
        // header's text stops left of it
        int white = 0, black = 0;
        for (int y = 2; y < 24; ++y)
            for (int x = 966; x < 1022; ++x) {
                const std::uint32_t v = at(x, y);
                if (v == 0xFFFFFF) ++white; else if (v == 0) ++black;
            }
        check(white > 40 && black > 600, "the title bar: a close button (an X) at the right");
        bool headerClear = true;
        for (int y = 2; y < 24; ++y)
            for (int x = 953; x < 962; ++x)
                if (at(x, y) == 0) headerClear = false;
        check(headerClear, "... the header's text stops before it");
        check(hipi::page_closeHit(1000, 10) && hipi::page_closeHit(965, 35) && !hipi::page_closeHit(900, 10) &&
              !hipi::page_closeHit(1000, 60), "page_closeHit(): the X (and a little below), nothing else");
        // The menu button, bottom right: its yellow frame there
        int frame = 0;
        for (int y = hipi::kPageMenuY; y < hipi::kPageMenuY + hipi::kPageMenuH; ++y)
            for (int x = hipi::kPageMenuX; x < hipi::kPageMenuX + hipi::kPageMenuW; ++x)
                if (at(x, y) != 0 && at(x, y) != 0xFFFFFF) ++frame;
        check(frame > 400 && hipi::page_menuHit(1010, 590) && !hipi::page_menuHit(900, 590),
              "the menu button bottom right (its touch area round it)");
    }

    // ── The details box: a tap on a card ────────────────────────────────
    // (three devices: TFPLOT's card at the bottom, about x 380..566)
    hipi::loopmap_drawAll();
    {
        int x = 0, y = 0;
        check(hipi::loopmap_cardPoint(static_cast<int>(plot.addr()), x, y) && x >= 380 && x < 566 && y > 450 &&
              !hipi::loopmap_cardPoint(29, x, y), "loopmap_cardPoint(): the middle of TFPLOT's card; none for 29");
    }
    check(hipi::loopmap_tap(470, 510) && hipi::loopmap_detailsOpen(), "a tap on TFPLOT's card: its details box");
    snapshot("loopmap_4_details.png");
    chip.render(panel.data(), 0.0, false);
    // The yellow GO TO PLOTTER VIEW button at the box's lower right
    const std::uint32_t yellow = at(10, 10);
    const int btn = inBox(512, 380, 752, 425, yellow);
    std::printf("     view button: %d yellow px\n", btn);
    check(btn > 3000, "... with a GO TO PLOTTER VIEW button");
    check(hipi::loopmap_tap(500, 300) && hipi::loopmap_detailsOpen(), "a tap in the box (not a button): stays open");
    check(hipi::loopmap_tap(640, 400) && viewOpened == &plot && !hipi::loopmap_detailsOpen(),
          "the button: the plotter's view (and the box closed)");
    // X CLOSE, a tap beside the box, and the strip's OK / X
    viewOpened = nullptr;
    hipi::loopmap_drawAll();
    hipi::loopmap_tap(470, 510);
    check(hipi::loopmap_tap(330, 400) && !hipi::loopmap_detailsOpen() && viewOpened == nullptr, "X CLOSE: closed");
    // An HP-GL error: its number and text in the Status row (before: none)
    {
        auto statusRow = [] {
            chip.render(panel.data(), 0.0, false);
            std::vector<std::uint32_t> row;
            for (int y = 280; y < 310; ++y) for (int x = 380; x < 760; ++x) row.push_back(at(x, y));
            return row;
        };
        check(plot.hpgl().errorCode() == 0, "TFPLOT: no error yet");
        hipi::loopmap_drawAll();
        hipi::loopmap_tap(470, 510);
        const std::vector<std::uint32_t> before = statusRow();
        hipi::loopmap_closeDetails();
        for (char ch : std::string("SC 0,100;")) plot.hpgl().put(ch);
        check(plot.hpgl().errorCode() == 2, "SC 0,100: error 2 (wrong number of parameters)");
        hipi::loopmap_drawAll();
        hipi::loopmap_tap(470, 510);
        snapshot("loopmap_5b_error.png");
        check(statusRow() != before, "... and the details box's Status row shows it");
        hipi::loopmap_closeDetails();
        plot.hpgl().clearError();
    }
    hipi::loopmap_tap(470, 510);
    check(hipi::loopmap_tap(60, 60) && !hipi::loopmap_detailsOpen(), "a tap beside the box: closed");
    hipi::loopmap_tap(470, 510);
    hipi::loopmap_detailsOk();
    check(viewOpened == &plot, "OK on the strip: the view");
    // A device without a view of its own (one after HIPI): no button, OK does nothing
    viewOpened = nullptr;
    frame(TAD + 5);
    frame(UNT);
    run(600);
    hipi::loopmap_drawAll();
    {
        int cx = 0, cy = 0;                   // (four devices now: card 5's middle)
        hipi::loopmap_cardPoint(5, cx, cy);
        hipi::loopmap_tap(cx, cy);
    }
    chip.render(panel.data(), 0.0, false);
    snapshot("loopmap_5_no_view.png");
    check(hipi::loopmap_detailsOpen() && inBox(512, 380, 752, 425, yellow) == 0, "a device after HIPI: no view button");
    hipi::loopmap_detailsOk();
    check(viewOpened == nullptr && hipi::loopmap_detailsOpen(), "... OK does nothing");
    hipi::loopmap_closeDetails();
    check(!hipi::loopmap_detailsOpen(), "X on the strip: closed");

    // ── Timing in the title bar: HIPI's time per frame, the loop's ──────
    // The loop: the shortest gap between frames as they pass (not IDY) --
    // here HIPI's plotter answering SDI, 850 us round the loop each
    {
        run(2100);                             // (what came before: shown, a new 2 s begins)
        frame(TAD + 3);
        const char id[] = "HP7470A\r\n";
        hipi::analyzer_capture(SDI, static_cast<IL_CMD_t>(id[0]));
        for (int i = 1; id[i] != 0; ++i) {
            fakeNowUs += 850;
            hipi::analyzer_capture(static_cast<IL_CMD_t>(id[i - 1]), static_cast<IL_CMD_t>(id[i]));
        }
        fakeNowUs += 850;
        hipi::analyzer_capture(static_cast<IL_CMD_t>('\n'), ETO);
        frame(UNT);
        // IDY polling, close together: not the loop (the controller's pace)
        for (int i = 0; i < 10; ++i) { hipi::analyzer_capture(IDY, IDY); fakeNowUs += 200; }
        // HIPI's time per frame (hipi.cpp tells it): 40, 60 and once 3.1 ms
        hipi::loopmap_noteHipiUs(40);
        hipi::loopmap_noteHipiUs(60);
        hipi::loopmap_noteHipiUs(3100);
        hipi::loopmap_drawAll();
        chip.render(panel.data(), 0.0, false);
        std::vector<std::uint32_t> title(panel.begin(), panel.begin() + 26 * 1024);
        run(2100);                             // the 2 s are over: shown
        hipi::LoopTiming tm = hipi::loopmap_timing();
        std::printf("     loop %u us (shortest), HIPI %u us (max %u)\n", static_cast<unsigned>(tm.loopMinUs),
                    static_cast<unsigned>(tm.hipiAvgUs), static_cast<unsigned>(tm.hipiMaxUs));
        check(tm.loopValid && tm.loopMinUs == 850, "loop: the shortest gap between frames (850 us; IDY 200 us apart left out)");
        check(tm.hipiValid && tm.hipiAvgUs == 1066 && tm.hipiMaxUs == 3100, "HIPI: its time per frame, average and longest");
        chip.render(panel.data(), 0.0, false);
        check(!std::equal(title.begin(), title.end(), panel.begin()), "... the title bar shows them by itself");
        run(600);                              // (the title: at most every 0.5 s)
        snapshot("loopmap_8_timing.png");
        // Quiet for a while: still shown
        run(5000);
        tm = hipi::loopmap_timing();
        check(tm.loopValid && tm.loopMinUs == 850 && tm.hipiValid, "... and kept while it's quiet");
        // CON: a frame of the PC's out on the loop and back -- exact
        hipi::loopmap_noteConRoundUs(1300);
        hipi::loopmap_noteConRoundUs(1320);
        run(2100);
        tm = hipi::loopmap_timing();
        check(tm.conValid && tm.conAvgUs == 1310, "CON: the loop's exact time (out and back), averaged");
    }

    // ── Six devices: the right side's two cards spread over it, centred
    // (they sat up and down in the corners), nothing in the menu button's corner
    frame(TAD + 6); frame(UNT);
    frame(TAD + 7); frame(UNT);
    run(600);
    hipi::loopmap_drawAll();
    {
        std::vector<std::pair<int, int>> right, all;
        for (int a = 1; a <= 30; ++a) {
            int cx = 0, cy = 0;
            if (!hipi::loopmap_cardPoint(a, cx, cy)) continue;
            all.push_back({ cx, cy });
            if (cx > 880) right.push_back({ cx, cy });
        }
        std::printf("     %zu cards, %zu on the right side", all.size(), right.size());
        for (auto& p : right) std::printf("  (%d,%d)", p.first, p.second);
        std::printf("\n");
        snapshot("loopmap_7_six.png");
        const int mid = (94 + 512) / 2;       // the ring's right side: T..B (full cards)
        check(all.size() == 6 && right.size() == 2 && std::abs((right[0].second + right[1].second) / 2 - mid) <= 4 &&
              std::abs(right[1].second - right[0].second) > 150 && std::min(right[0].second, right[1].second) > 94 + 88 &&
              std::max(right[0].second, right[1].second) < 512 - 88,
              "six devices: the right side's two spread evenly over it, clear of the corners");
        chip.render(panel.data(), 0.0, false);
        bool clear = true;
        for (auto& p : all)
            if (p.first + 93 > hipi::kPageMenuTouchX && p.second + 44 > hipi::kPageMenuTouchY) clear = false;
        check(clear, "... no card in the menu button's corner");
    }

    // ── The demo's made-up loop (demo_content.h): HIPI's three, 3 behind
    // PILBOX, 3 after -- all asked for their IDs
    hipi::loopmap_reset();
    hipi::analyzer_clear();
    auto round = [](IL_CMD_t f) { return hipi::demo::fakeLoopRound(f, hipiRound); };
    round(IFC);
    round(AAU);
    const IL_CMD_t aad = round(AAD + 1);
    const int n = static_cast<int>(aad - AAD) - 1;
    for (const hipi::demo::LoopAct& a : hipi::demo::fakeSessionActs(n, disp.addr(), prt.addr())) {
        IL_CMD_t last = round(a.frame);
        for (int e = 0; a.echo && IS_DATA(last) && e < 40; ++e) last = round(last);
        fakeNowUs += 30000;
        hipi::loopmap_poll();
    }
    run(600);
    hipi::loopmap_drawAll();
    snapshot("loopmap_6_demo.png");
    check(n == 9, "the demo's loop: 9 devices (HIPI's 3, PILBOX's 3, then 3 after)");

    std::printf("%s\n", failures ? "FAILED" : "ALL PASSED");
    return failures ? 1 : 0;
}
