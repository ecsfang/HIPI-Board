// demo_content.h -- what the demo (demo.h) shows: the texts for the
// display, the plots (HP-GL) and the printouts. Plain data, no hardware,
// so they can be looked at on the PC (tools/hipiview demo_view_test).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace hipi {
namespace demo {

// ── Display (HP 82163, 32 columns) ──────────────────────────────────────
std::string textIntro();            // title, a program listing
std::string textEscapes();          // cursor addressing, inverse video
// ── Display as an HP 92198 (80 columns) ─────────────────────────────────
std::string text80();               // a table, inverse video
std::string text80Roman8();         // the Roman-8 set (ESC j)
std::string text40();               // 40 x 20
// DIR of a cassette, from lifDescribe()'s lines (lif_info.hpp), as the
// HP-41 shows it on the 32-column display
std::string textDir(const std::string& file, const std::vector<std::string>& lifLines);

// ── Plotter (HP-GL, the 7470A's page: 250..10250 x 279..7479) ──────────
// The HIPI logo, hatched in the pens nearest its colours, with the name
// (the logo: RGB565, w x h, black = background)
std::string hpglLogo(const std::uint16_t* logo, int w, int h);
std::string hpglSpirograph();
std::string hpglFunctionPlot();
// ... in two parts: the axes, a short piece at a time from the origin out
// (a Fit view grows with them), then ticks, numbers, curves and labels
std::string hpglFunctionAxes();
std::string hpglFunctionCurves();
std::string hpglLoop();             // an HP-IL loop: controller and devices
// The function plot's SIN(X)/X at x = 0 (its top): where the demo digitizes
constexpr int kFunctionPeakX = 1200, kFunctionPeakY = 3879 + 2800;   // (as in hpglFunctionCurves())
// The digitized point marked: a circle round it and its coordinates
std::string hpglDigitizedMark(int x, int y);

// ── A made-up HP-IL loop (analyzer, loop map) ───────────────────────────
// HIPI's own devices, three of a PC program behind PILBOX, three after
// HIPI -- made up (not every detail as a real loop would have
// it: it's for showing). A device takes an address (AAD), and as talker
// answers SAI, SDI, SST and SDA, a byte each time the last one came round.
//
// One frame from the controller once round the loop. hipi: HIPI's own
// devices (hipi_simFrame(), which also shows the frame to the analyzer),
// handed the PC's devices to run after them.
using HipiRound = unsigned short (*)(unsigned short frame, unsigned short (*pcDevices)(unsigned short));
unsigned short fakeLoopRound(unsigned short frame, HipiRound hipi);
// The frames of the session after the auto-addressing (n devices in
// all): every device asked for its accessory and device ID, lines to the
// display and the printer (address 0: none), the status of a few, the
// voltmeter read. echo: an answer comes back -- send what returns round
// again until it's no longer data.
struct LoopAct { unsigned short frame; bool echo; };
std::vector<LoopAct> fakeSessionActs(int n, int displayAddr, int printerAddr);

// ── Printer (HP 82162A, 8-bit mode) ─────────────────────────────────────
// One printed line per entry (each ends with CR)
std::vector<std::string> printLines();

}  // namespace demo
}  // namespace hipi
