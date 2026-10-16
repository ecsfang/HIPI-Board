// plotter_test.cpp -- the HP-GL parser (plotter.cpp) on the PC, without a
// display: HP-GL in, the drawn segments checked.
//
//   CR and LF are ignored outside labels -- also in the middle of a number.
//   An HP-41 program that sends a long PA line in pieces puts CR LF
//   wherever its line ends ("...,2599,1557,26" CR LF "46,1569,...": the
//   x is 2646); the rest of such a line used to be lost. A command still
//   open at the end of the text is carried out when nothing more comes.
//
//   make plotter_test && ./plotter_test   (in tools/hipiview)
#include "plotter.h"
#include <cstdio>
#include <string>
#include <vector>

std::uint64_t fakeNowUs = 1000000;
bool bTrace = false, bExtTrace = false;

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}
void send(CPlotter& p, const std::string& s) { for (char c : s) p.put(c); }
bool has(const CPlotter& p, int x0, int y0, int x1, int y1) {
    for (const PlotSegment& s : p.segments())
        if (s.x0 == x0 && s.y0 == y0 && s.x1 == x1 && s.y1 == y1) return true;
    return false;
}
}  // namespace

int main() {
    {
        // From an HP-41 drawing a space shuttle (a nose cone arc): CR LF
        // in the middle of a coordinate
        CPlotter p("TFPLOT");
        send(p, "IN;SP1;PU;PA2187,1648;PD;"
                "PA2227,1622,2270,1599,2314,1581,2360,1566,2407,1556,2455,1550,2503,1548,2551,1550,2599,1557,26\r\n"
                "46,1569,2692,1584,2736,1604;\r\nPU;");
        std::printf("     %zu segments\n", p.segments().size());
        check(p.segments().size() == 12, "a PA line broken by CR LF: all 12 segments");
        check(has(p, 2599, 1557, 2646, 1569), "... the number split by CR LF read as one (2646)");
        check(has(p, 2692, 1584, 2736, 1604), "... and the points after it drawn");
    }
    {
        // CR / LF between commands, between the letters, after a separator
        CPlotter p("TFPLOT");
        send(p, "IN;SP1;\r\nP\r\nU;PA1000,1000;PD;PA1100,\r\n1200;\nPA1300\r\n,1400;PU;");
        check(has(p, 1000, 1000, 1100, 1200) && has(p, 1100, 1200, 1300, 1400) && p.segments().size() == 2,
              "CR / LF anywhere else outside a label: ignored too");
    }
    {
        // The manual's BOX (8.1): OUTA ends every text with CR LF; the
        // last command ("PU") has nothing after it -- carried out once the
        // HP-41 has been quiet a moment (idle())
        CPlotter p("TFPLOT");
        send(p, "IN SP1 PU1000 1000\r\nPD4000 1000 4000 4000\r\nPD1000 4000 1000 1000\r\nPU\r\n");
        check(p.segments().size() == 4, "BOX: the square, four sides");
        check(p.penDown(), "... PU at the end of the text: not yet (it might go on)");
        fakeNowUs += 100000;
        p.idle();
        check(p.penDown(), "... still not after 0.1 s");
        fakeNowUs += 300000;
        p.idle();
        check(!p.penDown(), "... carried out when nothing more came (0.4 s)");
        // A line that goes on in time isn't cut
        send(p, "PD;PA2000,2000,30\r\n");
        fakeNowUs += 50000;
        p.idle();
        send(p, "00,2500;");
        check(has(p, 2000, 2000, 3000, 2500), "... a number going on after CR LF (in time): one number");
    }
    {
        // In a label CR and LF are what they are: a new line
        CPlotter p("TFPLOT");
        send(p, "IN;SP1;PA1000,1000;LBAB\r\nCD\x03;");
        int minY = 32767, maxY = -32768;
        for (const PlotSegment& s : p.segments()) { minY = std::min<int>(minY, std::min(s.y0, s.y1)); maxY = std::max<int>(maxY, std::max(s.y0, s.y1)); }
        std::printf("     label: %zu strokes, y %d..%d\n", p.segments().size(), minY, maxY);
        check(!p.segments().empty() && minY < 1000 - 50, "LB: CR LF still start a new line (below the first)");
    }
    std::printf("%s\n", failures ? "FAILED" : "ALL PASSED");
    return failures ? 1 : 0;
}
