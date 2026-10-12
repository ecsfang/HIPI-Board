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
std::string hpglLoop();             // an HP-IL loop: controller and devices

// ── Printer (HP 82162A, 8-bit mode) ─────────────────────────────────────
// One printed line per entry (each ends with CR)
std::vector<std::string> printLines();

}  // namespace demo
}  // namespace hipi
