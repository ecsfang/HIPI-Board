// plotter_test.cpp -- HP-GL (hpgl.cpp) and the plotter on HP-IL
// (plotter.cpp) on the PC, without a display: HP-GL in, the drawn segments
// checked.
//
//   CR and LF are ignored outside labels -- also in the middle of a number.
//   An HP-41 program that sends a long PA line in pieces puts CR LF
//   wherever its line ends ("...,2599,1557,26" CR LF "46,1569,...": the
//   x is 2646); the rest of such a line used to be lost. A command still
//   open at the end of the text is carried out when nothing more comes.
//
//   make plotter_test && ./plotter_test   (in tools/hipiview)
#include "plotter.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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
void send(CPlotter& p, const std::string& s) { for (char c : s) p.hpgl().put(c); }
bool has(const CPlotter& p, int x0, int y0, int x1, int y1) {
    for (const PlotSegment& s : p.hpgl().segments())
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
        std::printf("     %zu segments\n", p.hpgl().segments().size());
        check(p.hpgl().segments().size() == 12, "a PA line broken by CR LF: all 12 segments");
        check(has(p, 2599, 1557, 2646, 1569), "... the number split by CR LF read as one (2646)");
        check(has(p, 2692, 1584, 2736, 1604), "... and the points after it drawn");
    }
    {
        // CR / LF between commands, between the letters, after a separator
        CPlotter p("TFPLOT");
        send(p, "IN;SP1;\r\nP\r\nU;PA1000,1000;PD;PA1100,\r\n1200;\nPA1300\r\n,1400;PU;");
        check(has(p, 1000, 1000, 1100, 1200) && has(p, 1100, 1200, 1300, 1400) && p.hpgl().segments().size() == 2,
              "CR / LF anywhere else outside a label: ignored too");
    }
    {
        // The manual's BOX (8.1): OUTA ends every text with CR LF; the
        // last command ("PU") has nothing after it -- carried out once the
        // HP-41 has been quiet a moment (idle())
        CPlotter p("TFPLOT");
        send(p, "IN SP1 PU1000 1000\r\nPD4000 1000 4000 4000\r\nPD1000 4000 1000 1000\r\nPU\r\n");
        check(p.hpgl().segments().size() == 4, "BOX: the square, four sides");
        check(p.hpgl().penDown(), "... PU at the end of the text: not yet (it might go on)");
        fakeNowUs += 100000;
        p.idle();
        check(p.hpgl().penDown(), "... still not after 0.1 s");
        fakeNowUs += 300000;
        p.idle();
        check(!p.hpgl().penDown(), "... carried out when nothing more came (0.4 s)");
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
        for (const PlotSegment& s : p.hpgl().segments()) { minY = std::min<int>(minY, std::min(s.y0, s.y1)); maxY = std::max<int>(maxY, std::max(s.y0, s.y1)); }
        std::printf("     label: %zu strokes, y %d..%d\n", p.hpgl().segments().size(), minY, maxY);
        check(!p.hpgl().segments().empty() && minY < 1000 - 50, "LB: CR LF still start a new line (below the first)");
    }
    {
        // IP / SC: user units. A test program's circle, as it sent it
        // (log 2026-10-18): IP1000,1000,6000,6000 and SC0,25000,0,25000,
        // then 37 points on a circle around (12500,12500), r = 4500 user
        // units -- padded numbers with decimals. Without SC the points
        // (up to 17000) were taken as plotter units: off the paper, nothing
        // drawn.
        CPlotter p("HP7470A");
        std::string hpgl = "IN;SP1;IP1000,1000,6000,6000;\r\nSC0,25000,0,25000;\r\n";
        for (int i = 0; i <= 36; ++i) {
            const double a = i * 3.14159265358979 / 18.0;
            char line[96];
            std::snprintf(line, sizeof(line), "PA%20s%-20.12g,%20s%-20.12g;PD;\r\n", "",
                          12500 + 4500 * std::cos(a), "", 12500 + 4500 * std::sin(a));
            hpgl += line;
        }
        hpgl += "PU;SP0";
        send(p, hpgl);
        check(p.hpgl().segments().size() == 36, "IP + SC: the circle, all 36 segments");
        int minX = 99999, maxX = -1, minY = 99999, maxY = -1;
        for (const PlotSegment& s : p.hpgl().segments()) {
            minX = std::min<int>(minX, std::min(s.x0, s.x1)); maxX = std::max<int>(maxX, std::max(s.x0, s.x1));
            minY = std::min<int>(minY, std::min(s.y0, s.y1)); maxY = std::max<int>(maxY, std::max(s.y0, s.y1));
        }
        std::printf("     x %d..%d  y %d..%d\n", minX, maxX, minY, maxY);
        check(minX == 2600 && maxX == 4400 && minY == 2600 && maxY == 4400,
              "... centred on (3500,3500), radius 900 plotter units");
        check(has(p, 4400, 3500, 4386, 3656), "... user units rounded to the nearest plotter unit");
    }
    {
        CPlotter p("HP7470A");
        // PR in user units: only scaled; IP with two numbers moves P2 along
        send(p, "IN;IP1000,1000,2000,2000;SC0,100,0,100;PA10,10;PD;PR20,0,0,30;PU;");
        check(has(p, 1100, 1100, 1300, 1100) && has(p, 1300, 1100, 1300, 1400), "SC: PR deltas scaled too");
        send(p, "IP2000,2000;PA0,0;PD;PA100,100;PU;");
        check(has(p, 2000, 2000, 3000, 3000), "IP x,y: P1 moved, P2 along; SC follows");
        send(p, "SC;PA5000,5000;PD;PA5100,5000;PU;");
        check(has(p, 5000, 5000, 5100, 5000), "bare SC: plotter units again");
        send(p, "SC0,10,0,10;DF;PA6000,6000;PD;PA6100,6000;PU;");
        check(has(p, 6000, 6000, 6100, 6000), "DF: user units off");
        send(p, "SC0,10,0,10;IN;PA700,700;PD;PA800,700;PU;");
        check(has(p, 700, 700, 800, 700), "IN: user units off, P1/P2 back");
    }

    {
        // CI / AA from the same test program (log 2026-10-18 13:08): a big
        // circle and four small ones around it, in user units
        CPlotter p("HP7470A");
        send(p, "IN;SP1;IP2650,1325,7650,6325;\r\nSC-1000,1000,-1000,1000;\r\nPA-800,800;\r\n"
                "CI50;PR600,0;CI50;PR-300,-300;CI250;\r\nPR-300,-300;CI50;PR600,0;CI50;\r\n");
        check(p.hpgl().segments().size() == 5 * 72, "CI: five circles, 72 chords each (5 degrees)");
        // The first: centre (-800,800) -> (3150,5825), r 50 -> 125 plotter units
        check(has(p, 3275, 5825, 3275, 5836), "... the first starts at 0 degrees, r 125 around (3150,5825)");
        check(p.hpgl().penX() == 4650 && p.hpgl().penY() == 4325 && !p.hpgl().penDown(), "... the pen back at the last centre, up");
        int minX = 99999, maxX = -1;
        for (const PlotSegment& sg : p.hpgl().segments()) {
            minX = std::min<int>(minX, std::min(sg.x0, sg.x1)); maxX = std::max<int>(maxX, std::max(sg.x0, sg.x1));
        }
        check(minX == 3025 && maxX == 4775, "... the big one (r 625) and the small ones within x 3025..4775");
    }
    {
        // A rounded frame: AA with the pen down, then CI30 in the middle
        CPlotter p("HP7470A");
        send(p, "IN;SP1;IP2650,1325,7650,6325;\r\nSC0,100,0,100;\r\nPA0,20;\r\n"
                "PD;PA0,40;AA0,50,180;PA0,80;\r\n");
        check(p.hpgl().penX() == 2650 && p.hpgl().penY() == 1325 + 80 * 50, "AA: the pen at the arc's end, then on");
        check(has(p, 2650, 2325, 2650, 3325) && has(p, 2650, 4325, 2650, 5325) && p.hpgl().segments().size() == 1 + 36 + 1,
              "AA0,50,180 from (0,40): 36 chords between the two lines");
        bool right = false;
        for (const PlotSegment& sg : p.hpgl().segments()) right = right || sg.x1 == 2650 + 500;
        check(right, "... counter-clockwise: through (10,50), right of the centre");
        send(p, "PU;PA50,50;AR0,10,90;");
        check(p.hpgl().segments().size() == 38, "AR with the pen up: only moves");
    }

    {
        // DP / OD / DC and the status byte (OS)
        CPlotter p("HP7470A");
        send(p, "IN;OS;");
        check(p.outputText() == "24\r\n", "OS after IN: 24 (initialized, ready)");
        send(p, "IP1000,1000,6000,6000;OS;");
        check(p.outputText() == "18\r\n", "OS after IP: bit 1, P1/P2 changed (16 + 2)");
        send(p, "OS;");
        check(p.outputText() == "16\r\n", "... cleared by reading it");
        send(p, "PU;DP;");
        check(p.hpgl().digitizing(), "DP: waiting for a point");
        p.hpgl().digitize(3000, 2000);
        check(!p.hpgl().digitizing() && p.hpgl().penX() == 3000 && p.hpgl().penY() == 2000, "a point: the pen there, no longer waiting");
        send(p, "OS;");
        check(p.outputText() == "20\r\n", "OS: bit 2, a point is ready (16 + 4)");
        send(p, "OD;");
        check(p.outputText() == "3000,2000,0\r\n", "OD: x,y,pen");
        send(p, "OS;");
        check(p.outputText() == "16\r\n", "... and OS bit 2 cleared by OD");
        send(p, "DP;DC;");
        p.hpgl().digitize(1, 1);
        check(!p.hpgl().digitizing() && p.hpgl().penX() == 3000, "DC: no longer waiting, a later point ignored");
        send(p, "DP;");
        p.hpgl().digitize(20000, -5);
        send(p, "OD;");
        check(p.outputText() == "10300,0,0\r\n", "a point off the plotter: kept to its area");
    }
    {
        // Not implemented: logged once (since IN), and listed in the status
        CPlotter p("HP7470A");
        send(p, "IN;QQ1;PA100,100;QR;QQ;");
        check(p.hpgl().unsupportedSeen() == "QQ QR ", "commands not implemented: noted once each");
        send(p, "IN;");
        check(p.hpgl().unsupportedSeen().empty(), "... and again after IN");
    }

    {
        // A test program's label plot (log 2026-10-18 15:35): text from
        // x=100, left of P1 -- not cut (IW: the whole area); after IP the
        // default (SR) size follows the new P1..P2; SI's doesn't
        CPlotter p("HP7470A");
        send(p, "IN;SP1;PA100,7000;LBD\x03");
        int minX = 99999;
        for (const PlotSegment& sg : p.hpgl().segments()) minX = std::min<int>(minX, std::min(sg.x0, sg.x1));
        check(minX == 100, "LB at x=100 (left of P1): the letter whole, from x=100");
        const std::size_t before = p.hpgl().segments().size();
        int w0 = 0;
        for (const PlotSegment& sg : p.hpgl().segments()) w0 = std::max<int>(w0, std::max(sg.x0, sg.x1));
        send(p, "IP 1000,1000,7000,7000;PA100,6500;LBD\x03");
        int w1 = 0;
        for (std::size_t i = before; i < p.hpgl().segments().size(); ++i)
            w1 = std::max<int>(w1, std::max(p.hpgl().segments()[i].x0, p.hpgl().segments()[i].x1));
        std::printf("     D: %d units wide before IP, %d after\n", w0 - 100, w1 - 100);
        check(w1 - 100 < w0 - 100 && w1 - 100 >= (w0 - 100) * 6 / 10 - 2 && w1 - 100 <= (w0 - 100) * 6 / 10 + 2,
              "IP 1000,1000,7000,7000: the default size 0.6x as wide (P1..P2 6000 instead of 10000)");
        CPlotter q("HP7470A");
        send(q, "IN;SI0.1875,0.27;IP1000,1000,7000,7000;PA100,100;LBD\x03");
        int w2 = 0;
        for (const PlotSegment& sg : q.hpgl().segments()) w2 = std::max<int>(w2, std::max(sg.x0, sg.x1));
        check(w2 - 100 == w0 - 100, "SI (absolute): IP leaves the size alone");
        send(q, "OW;");
        check(q.outputText() == "0,0,10300,7650\r\n", "OW after IN: the whole area");
    }

    {
        // The sales chart (log 2026-10-18 15:58): CP counts character
        // cells (1.5 widths, 2 heights), from where the last label ended;
        // "CP-9.-1" is -9 and -1; LT dashes the lines
        CPlotter p("HP7470A");
        auto near = [](int v, int want) { return v >= want - 2 && v <= want + 2; };
        send(p, "IN;PA1000,1000;LBAB\x03");
        // default size: 75 x 108 units; a cell 112.5 x 216
        check(near(p.hpgl().penX(), 1225) && p.hpgl().penY() == 1000, "LB: the pen ends after the label (2 cells)");
        send(p, "CP-2,-1;");
        check(near(p.hpgl().penX(), 1000) && p.hpgl().penY() == 784, "CP-2,-1 from there: back to the start, one line (2 heights) down");
        send(p, "PA2000,2000;LBSALES $\x03" "CP-9.-1;");
        check(p.hpgl().penX() == 2000 && p.hpgl().penY() == 2000 - 216, "\"CP-9.-1\" (two numbers): the pen to the line's start, a line down");
        const std::size_t n0 = p.hpgl().segments().size();
        send(p, "LBL\x03");
        int lx = 99999;
        for (std::size_t i = n0; i < p.hpgl().segments().size(); ++i) lx = std::min<int>(lx, std::min(p.hpgl().segments()[i].x0, p.hpgl().segments()[i].x1));
        std::printf("     L after CP-9,-1 starts at x %d\n", lx);
        check(near(lx, 2000 - 1013 + 1), "... and the next label 9 spaces left of the CR point (2000 - 9 x 112.5)");
        CPlotter q("HP7470A");
        send(q, "IN;PA1000,1000 2000 1000 3000,1000;");
        send(q, "PA1000,2000;PD;PA1000 2000 2000 2000;PU;");
        check(has(q, 1000, 2000, 2000, 2000), "PA x,y x y: a space after a comma still separates");
        CPlotter r("HP7470A");
        send(r, "IN;LT2;PA1000,1000;PD;PA2000,1000;PU;");
        // P1..P2 diagonal 12322 -> pattern 4 % = 493 units: 246 drawn, 246 not
        check(r.hpgl().segments().size() == 3 && has(r, 1000, 1000, 1246, 1000) && has(r, 1493, 1000, 1739, 1000),
              "LT2: half dashes, 4 % of the diagonal (493 units)");
        CPlotter t("HP7470A");
        send(t, "IN;LT2;PA1000,1000;PD;PA1100,1000,1200,1000,1300,1000;PU;");
        check(t.hpgl().segments().size() == 3 && has(t, 1200, 1000, 1246, 1000),
              "LT: the pattern carries on from one line to the next");
        send(t, "LT;PA1000,3000;PD;PA2000,3000;PU;");
        check(has(t, 1000, 3000, 2000, 3000), "bare LT: solid again");
    }

    {
        // SM: a character at every point (its argument one character, as
        // DT); OE / OS bit 5 for an unknown instruction; DR
        CPlotter p("HP7470A");
        send(p, "IN;SM*;PA1000,1000,2000,1000;SM;PA3000,1000;");
        const std::size_t withSymbols = p.hpgl().segments().size();
        check(withSymbols > 0 && p.hpgl().penX() == 3000, "SM*: symbols drawn at the points, the pen moves on");
        send(p, "PA4000,1000;");
        check(p.hpgl().segments().size() == withSymbols, "SM; -- off: no more symbols");
        send(p, "OS;");
        check(p.outputText() == "24\r\n", "no error: OS bit 5 clear");
        send(p, "ZQ;OS;");
        check(p.outputText() == "48\r\n", "an unknown instruction: OS bit 5 (16 + 32)");
        send(p, "OE;");
        check(p.outputText() == "1\r\n", "OE: 1 (instruction not recognized)");
        send(p, "OE;OS;");
        check(p.outputText() == "16\r\n", "... cleared by reading OE");
        send(p, "IN;IP0,0,10000,5000;DR0,100;PA1000,1000;LBI\x03");
        check(p.hpgl().penX() == 1000 && p.hpgl().penY() > 1000, "DR0,100: the label runs upwards");
        send(p, "VS10;IM233;CS0;OE;");
        check(p.outputText() == "0\r\n", "VS, IM, CS0: accepted, no error");
    }

    {
        // Character sets (CS/CA, SS/SA, SO/SI): the national glyphs, and the
        // accents overstrike the character before (the pen doesn't move on)
        CPlotter p("HP7470A");
        send(p, "IN;PA1000,1000;CS2;LBU{B\x03");
        check(p.hpgl().penX() == 1000 + 2 * 112 + 1 || p.hpgl().penX() == 1225, "CS2 \"U{B\": the umlaut over the U -- two cells, not three");
        bool high = false;
        for (const PlotSegment& sg : p.hpgl().segments()) high = high || std::min(sg.y0, sg.y1) > 1000 + 108;
        check(high, "... the umlaut drawn above the capital");
        CPlotter q("HP7470A");
        send(q, "IN;PA1000,1000;LB#\x03");
        const std::size_t ascii = q.hpgl().segments().size();
        send(q, "CS4;PA1000,3000;LB#\x03");
        check(q.hpgl().segments().size() != 2 * ascii, "CS4: '#' is another glyph (inverted question mark)");
        send(q, "CS7;OE;");
        check(q.outputText() == "3\r\n", "CS7: error 3 (sets 0..4)");
        CPlotter r("HP7470A");
        send(r, "IN;CA3;PA1000,1000;LBA\x0E|\x0FR\x03");
        check(r.hpgl().penX() == 1225 || r.hpgl().penX() == 1224 || r.hpgl().penX() == 1226, "CA3 + SO/SI in the label: the ring over the A (two cells)");
        send(r, "IP200,200,5000,5000,600;OE;");
        check(r.outputText() == "2\r\n", "IP with five numbers: error 2, ignored");
    }

    {
        // Errors as the manual has them (OE's numbers), the E-mask (IM),
        // the ERROR light (errorCode()) and HIPI's error log
        CPlotter p("TFPLOT");
        auto oe = [&](const std::string& hpgl) {
            send(p, "IN;" + hpgl + ";OE;");
            const std::string t = p.outputText();
            return t.empty() ? -1 : std::atoi(t.c_str());
        };
        check(oe("PA1000,1000,2000") == 2, "PA with an odd number: error 2");
        send(p, "IN;PD;PA1000,1000,2000;");
        check(p.hpgl().penX() == 1000 && p.hpgl().penY() == 1000, "... the pair before it still drawn to");
        check(oe("PA40000,1000") == 3, "PA beyond 32767: error 3");
        check(oe("SP40000") == 3, "SP 40000: error 3");
        check(oe("VS97") == 3 && oe("VS38") == 0, "VS 97: error 3, VS 38: fine");
        check(oe("SI1") == 2 && oe("SR1,2,3") == 2 && oe("SI0.5,0.8") == 0, "SI/SR: both or none (error 2)");
        check(oe("SR200,1") == 3 && oe("SL200") == 3 && oe("TL130") == 3, "SR/SL/TL beyond 127.9999: error 3");
        check(oe("DI0,0") == 3 && oe("DI1") == 2 && oe("DR0,0") == 3 && oe("DR1,0,5") == 2, "DI/DR: 0,0 error 3, one or three numbers error 2");
        check(oe("IW1,2,3") == 2 && oe("CI") == 2 && oe("AA1,2") == 2, "IW/CI/AA: wrong number: error 2");
        check(oe("LT200") == 3 && oe("LT9") == 0 && oe("LT-5") == 0, "LT 200: error 3; LT 9 ignored, LT -5 solid: no error");
        check(oe("AP;AF;AH;EC;VA;VN") == 0 && p.hpgl().unsupportedSeen().empty(), "AP AF AH EC VA VN: no-ops, no error");
        check(oe("ZZ") == 1, "ZZ: error 1");

        // The light: lit by an error, out after OE, IN or HIPI's Clear error
        send(p, "IN;SC0,100;");
        check(p.hpgl().errorCode() == 2, "the ERROR light: lit (error 2)");
        check(!p.hpgl().errorLog().empty() && p.hpgl().errorLog().back().code == 2 &&
              std::string(p.hpgl().errorLog().back().command) == "SC 0,100", "the log: error 2, \"SC 0,100\"");
        const std::uint32_t serial = p.hpgl().errorSerial();
        send(p, "OE;");
        check(p.hpgl().errorCode() == 0 && p.hpgl().errorSerial() != serial, "OE puts the light out (and the view hears of it)");
        send(p, "XX;");
        p.hpgl().clearError();
        check(p.hpgl().errorCode() == 0, "Clear error (HIPI's menu): out");
        send(p, "OE;");
        check(p.outputText() == "0\r\n", "... and OE says 0 then");
        send(p, "XX;IN;");
        check(p.hpgl().errorCode() == 0 && p.hpgl().errorLog().size() >= 3, "IN puts it out; the log is kept");

        // E-mask: IM 223 (default) leaves out error 6; IM 0 keeps nothing
        send(p, "IN;IM0;ZZ;OE;");
        check(p.outputText() == "0\r\n" && p.hpgl().errorCode() == 0, "IM0: an unknown instruction leaves no error");
        send(p, "IN;IM4;SC1;OE;");
        check(p.outputText() == "0\r\n", "IM4 (error 3 only): error 2 not kept");
        send(p, "IM4;SP40000;OE;");
        check(p.outputText() == "3\r\n", "... error 3 kept");
        send(p, "IN;SI1,1;CP0,-30000;CP0,-30000;OE;");
        check(p.outputText() == "0\r\n", "position overflow (error 6): not kept with the default mask");
        send(p, "IN;IM32;SI1,1;CP0,-30000;CP0,-30000;OE;");
        check(p.outputText() == "6\r\n", "... kept with IM 32");
        // DF puts the mask back too
        send(p, "IN;IM0;DF;ZZ;OE;");
        check(p.outputText() == "1\r\n", "DF: the E-mask back to 223");
        for (std::size_t i = 0; i < HpglInterpreter::kErrorLogSize + 2; ++i) send(p, "QQ;");
        check(p.hpgl().errorLog().size() == HpglInterpreter::kErrorLogSize, "the log keeps the last 8");
    }
    {
        // HP-IL: SDI "HP7470A" CR LF; SST the OS byte (bit 3 not cleared), then ETO
        CPlotter p("TFPLOT");
        p.setEnabled(true);
        p.hpil(AAU);
        p.hpil(AAD + 1);
        p.hpil(TAD + 1);
        std::string id;
        IL_CMD_t f = p.hpil(SDI);
        for (int i = 0; i < 20 && f != ETO; ++i) { id += static_cast<char>(f & 0xFF); f = p.hpil(f); }
        check(id == "HP7470A\r\n", "SDI: \"HP7470A\" CR LF");
        const IL_CMD_t st = p.hpil(SST);
        check(st == 24, "SST: 24 after power-on (initialized, ready)");
        check(p.hpil(st) == ETO, "... then ETO");
        const IL_CMD_t st2 = p.hpil(SST);
        p.hpil(st2);
        check(st2 == 24, "SST again: still 24 (SST doesn't clear bit 3, OS does)");
        p.hpil(UNT);
        p.hpil(LAD + 1);
        for (char c : std::string("ZZ;")) p.hpil(static_cast<IL_CMD_t>(static_cast<unsigned char>(c)));
        p.hpil(UNL);
        p.hpil(TAD + 1);
        const IL_CMD_t st3 = p.hpil(SST);
        p.hpil(st3);
        check(st3 == (24 | 32), "SST with an error: bit 5 (56)");
        p.hpil(UNT);
    }

    {
        // IW beyond the paper: lines still cut at the paper's edge (the
        // hard-clip limits) -- they used to be drawn off it
        CPlotter p("TFPLOT");
        send(p, "IN;IW-5000,-5000,20000,20000;SP1;PA10000,1000;PD;PA12000,1000;PU;PA1000,500;PD;PA1000,-500;PU;");
        check(p.hpgl().segments().size() == 2 && has(p, 10000, 1000, 10300, 1000) && has(p, 1000, 500, 1000, 0),
              "IW beyond the paper: cut at the paper's edge");
        send(p, "PA11000,8000;PD;PA12000,9000;PU;");
        check(p.hpgl().segments().size() == 2, "... a line all off the paper: not drawn");
        send(p, "OW;");
        check(p.outputText() == "-5000,-5000,20000,20000\r\n", "... OW still answers the window as given");
    }

    {
        // Plotter -> Errors: a row says what the error is, the command cut
        // if need be ("1 instruction not recognized: R~" didn't)
        const std::string r1 = HpglInterpreter::errorRow(1, "RO 90", 32);
        std::printf("     \"%s\"\n", r1.c_str());
        check(r1 == "1 unknown instruction: RO 90", "an error row: number, meaning, command");
        const std::string r2 = HpglInterpreter::errorRow(2, "PA 1000,1000,2000,3000,4000", 32);
        std::printf("     \"%s\"\n", r2.c_str());
        check(r2.size() == 32 && r2.rfind("2 wrong param. count: PA 1000", 0) == 0 && r2.back() == '~',
              "... a long command cut, the meaning kept");
        bool fits = true;
        for (int c : { 0, 1, 2, 3, 5, 6, 8, 9 })
            if (HpglInterpreter::errorRow(c, "XXXXXXXX", 32).find(": XXX") == std::string::npos) fits = false;
        check(fits, "... every meaning leaves room for the command");
    }

    std::printf("%s\n", failures ? "FAILED" : "ALL PASSED");
    return failures ? 1 : 0;
}
