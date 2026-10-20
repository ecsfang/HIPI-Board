// demo_content.cpp -- see demo_content.h
#include "demo_content.h"
#include "hpil.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace hipi {
namespace demo {

namespace {

constexpr double kPi = 3.14159265358979323846;

// Text with the high bit set: inverse video on the 82163 and the 92198
std::string inverse(const std::string& s) {
    std::string r;
    for (char c : s) r += static_cast<char>(static_cast<unsigned char>(c) | 0x80);
    return r;
}

// ESC % col row (82163 and 92198)
std::string at(int col, int row) {
    std::string s = "\x1b%";
    s += static_cast<char>(col);
    s += static_cast<char>(row);
    return s;
}

// HP-GL helpers
void pu(std::string& g, int x, int y) { g += "PU" + std::to_string(x) + "," + std::to_string(y) + ";"; }
void pd(std::string& g, int x, int y) { g += "PD" + std::to_string(x) + "," + std::to_string(y) + ";"; }
void pen(std::string& g, int p) { g += "SP" + std::to_string(p) + ";"; }
void label(std::string& g, int x, int y, double wCm, double hCm, const std::string& text) {
    char b[48];
    std::snprintf(b, sizeof(b), "SI%.2f,%.2f;", wCm, hCm);
    g += b;
    pu(g, x, y);
    g += "LB" + text + "\x03";
}
// A polyline from points (pen up to the first, down through the rest)
void poly(std::string& g, const std::vector<std::pair<int, int>>& pts) {
    if (pts.empty()) return;
    pu(g, pts[0].first, pts[0].second);
    g += "PD";
    for (std::size_t i = 1; i < pts.size(); ++i) {
        if (i > 1) g += ",";
        g += std::to_string(pts[i].first) + "," + std::to_string(pts[i].second);
    }
    g += ";PU;";
}
void box(std::string& g, int x0, int y0, int x1, int y1) {
    poly(g, { { x0, y0 }, { x1, y0 }, { x1, y1 }, { x0, y1 }, { x0, y0 } });
}
void circle(std::string& g, int cx, int cy, int r, int steps = 48) {
    std::vector<std::pair<int, int>> pts;
    for (int i = 0; i <= steps; ++i) {
        const double a = 2 * kPi * i / steps;
        pts.push_back({ cx + static_cast<int>(std::lround(r * std::cos(a))),
                        cy + static_cast<int>(std::lround(r * std::sin(a))) });
    }
    poly(g, pts);
}

}  // namespace

// ── Display ─────────────────────────────────────────────────────────────

std::string textIntro() {
    std::string s = "\x1b" "E";
    s += "HIPI - HP-IL PICO INTERFACE\r\n";
    s += "HP 82163 VIDEO INTERFACE\r\n";
    s += "PRP \"DEMO\"\r\n";
    const char* listing[] = {
        "01*LBL \"DEMO\"", "02 \"HELLO, HIPI\"", "03 AVIEW", "04 FIX 4", "05 PI", "06 STO 01",
        "07*LBL 01", "08 X^2", "09 PRX", "10 ISG 00", "11 GTO 01", "12 END",
    };
    for (const char* l : listing) { s += l; s += "\r\n"; }
    return s;
}

std::string textEscapes() {
    std::string s = "\x1b" "E";
    s += "CURSOR ADDRESSING (ESC %)\r\n";
    // A frame of stars drawn by addressing the cursor
    for (int c = 2; c < 30; ++c) { s += at(c, 3) + "*"; s += at(31 - c, 11) + "*"; }
    for (int r = 3; r <= 11; ++r) { s += at(2, 14 - r) + "*"; s += at(29, r) + "*"; }
    s += at(9, 6) + inverse(" INVERSE ");
    s += at(10, 8) + "VIDEO TOO";
    s += at(0, 13) + "ESC J/K CLEARS, ESC L/M LINES";
    return s;
}

std::string text80() {
    std::string s = "\x1b" "E";
    std::string head = " HP 92198 80-COLUMN VIDEO INTERFACE (MC00701A)";
    std::string right = "HIPI DEMO ";
    head.resize(80 - right.size(), ' ');
    s += inverse(head + right);
    s += "\r\n\r\n";
    const double regs[] = { 1.234567890, 2.718281828, 3.141592654, -0.577215665, 1.414213562, 0.693147181,
                            6.022140760, 1.602176634, 9.806650000, 2.997924580, 6.626070150, 1.380649000 };
    for (int row = 0; row < 3; ++row) {
        std::string line;
        for (int col = 0; col < 4; ++col) {
            const int r = row * 4 + col;
            char b[32];
            std::snprintf(b, sizeof(b), "R%02d=%12.9f", r, regs[r]);
            line += b;
            if (col < 3) line += "    ";
        }
        s += line + "\r\n";
    }
    s += "\r\n";
    s += "Long lines go on on the next line: the 92198 keeps 48 lines of 80 characters, and the "
         "screen is a window onto them.\r\n\r\n";
    s += "ASCII: 128-255 are " + inverse("inverse video") + ".\r\n";
    return s;
}

// Roman-8 is a mode for the whole screen (ESC j / ESC k): every byte
// 128-255 on it changes meaning, inverse video too -- so it gets a page of
// its own (and ESC k is left to text40()).
std::string text80Roman8() {
    std::string s = "\x1b" "E" "\x1bj";
    s += "\r\n   ROMAN-8 CHARACTER SET (ESC j)\r\n   -------------------------------\r\n\r\n";
    s += "   H\xCClsningar fr\xD4n G\xCEteborg!   \xD0\xD8\xDA \xD4\xCC\xCE\r\n\r\n";
    s += "   Gr\xCF\xDE" "e aus M\xCF" "nchen,  caf\xC5 cr\xC9me,  se\xB7or,  \xD3r\xD6sk\xD6" "bing\r\n\r\n";
    s += "   \xBB 12   \xBC 500   \xBF 25   \xBD 4   \xB9Qu\xC5?  \xB8Ol\xC5!   \xF8 + \xF7   20\xB3" "C\r\n";
    return s;
}

std::string text40() {
    std::string s = "\x1bk" "\x1b]";             // back to ASCII; 40 x 20 (clears)
    s += "\r\n" + inverse(" 40 x 20, DOUBLE WIDTH ") + "\r\n\r\n";
    s += "  ESC ] : 40 COLUMNS\r\n  ESC [ : 80 COLUMNS\r\n";
    return s;
}

std::string textDir(const std::string& file, const std::vector<std::string>& lifLines) {
    std::string s = "\x1b" "E";
    s += "CASSETTE: " + file.substr(0, 22) + "\r\n";
    s += "DIR\r\n";
    bool table = false;
    int n = 0;
    for (const std::string& l : lifLines) {
        if (!table) {
            if (l.rfind("NAME", 0) == 0) { table = true; s += "NAME       TYPE    SIZE\r\n"; }
            continue;
        }
        if (l.empty() || n >= 11) break;
        // lifDescribe()'s columns (name 0..9, type 11..20, size 21..28)
        // narrowed to the display: the type's first word, 6 wide ("BAS71"
        // of "BAS71 (SP)"), the size 5 wide
        auto trim = [](std::string t) {
            while (!t.empty() && t.front() == ' ') t.erase(t.begin());
            while (!t.empty() && t.back() == ' ') t.pop_back();
            return t;
        };
        std::string type = trim(l.size() > 11 ? l.substr(11, 10) : "");
        type = type.substr(0, std::min<std::size_t>(type.find(' '), 6));
        const std::string size = trim(l.size() > 21 ? l.substr(21, 8) : "");
        char row[40];
        std::snprintf(row, sizeof(row), "%-10.10s %-6s %5.5s", l.substr(0, 10).c_str(), type.c_str(), size.c_str());
        s += std::string(row) + "\r\n";
        ++n;
    }
    if (!table) s += "(NOT A LIF CASSETTE)\r\n";
    return s;
}

// ── Plotter ─────────────────────────────────────────────────────────────

std::string hpglLogo(const std::uint16_t* logo, int w, int h) {
    std::string g = "IN;";
    // The logo hatched: one stroke per run of coloured pixels in a row,
    // in the pen nearest the row's colour (black paper: 2 red, 3 blue,
    // 4 green, 5 yellow)
    const int x0 = 900, y0 = 1700, sizeH = 3900;
    const double k = static_cast<double>(sizeH) / h;
    int curPen = 0;
    for (int row = 0; row < h; ++row) {
        const int y = y0 + static_cast<int>(std::lround((h - 1 - row) * k));
        for (int x = 0; x < w;) {
            const std::uint16_t c = logo[row * w + x];
            const int r = (c >> 11) << 3, gr = ((c >> 5) & 0x3F) << 2, b = (c & 0x1F) << 3;
            if (r + gr + b < 96) { ++x; continue; }
            int e = x;
            while (e + 1 < w) {
                const std::uint16_t d = logo[row * w + e + 1];
                if ((((d >> 11) << 3) + (((d >> 5) & 0x3F) << 2) + ((d & 0x1F) << 3)) < 96) break;
                ++e;
            }
            // The nearest pen colour
            struct { int p, r, g, b; } pens[] = { { 2, 255, 0, 0 }, { 5, 255, 255, 0 }, { 4, 0, 255, 0 }, { 3, 0, 80, 255 } };
            int best = 2, bestD = 1 << 30;
            for (auto& p : pens) {
                const int d = (p.r - r) * (p.r - r) + (p.g - gr) * (p.g - gr) + (p.b - b) * (p.b - b);
                if (d < bestD) { bestD = d; best = p.p; }
            }
            if (best != curPen) { pen(g, best); curPen = best; }
            pu(g, x0 + static_cast<int>(std::lround(x * k)), y);
            pd(g, x0 + static_cast<int>(std::lround((e + 1) * k)), y);
            x = e + 1;
        }
    }
    g += "PU;";
    pen(g, 5);
    label(g, 4600, 4300, 1.6, 2.6, "HIPI");
    pen(g, 1);
    label(g, 4650, 3500, 0.45, 0.7, "HP-IL Pico Interface");
    pen(g, 3);
    label(g, 4650, 2750, 0.22, 0.38, "display  cassette  plotter  printer");
    pen(g, 4);
    label(g, 4650, 2250, 0.22, 0.38, "on a Raspberry Pi Pico 2");
    g += "PU;SP0;";
    return g;
}

std::string hpglSpirograph() {
    std::string g = "IN;";
    const int cx = 5250, cy = 3879;
    // Three hypotrochoids (a circle rolling inside another), each turned
    struct Curve { int pen; double R, r, d, turn; } curves[] = {
        { 3, 5, 3, 5, 0 }, { 2, 7, 4, 6, 0.3 }, { 4, 9, 5, 6.5, 0.6 } };
    for (const Curve& c : curves) {
        const double scale = 3300.0 / (c.R - c.r + c.d);
        const int turns = static_cast<int>(c.r);          // closes after r turns (integer radii)
        const int steps = 120 * turns;
        std::vector<std::pair<int, int>> pts;
        for (int i = 0; i <= steps; ++i) {
            const double t = 2 * kPi * turns * i / steps;
            const double x = (c.R - c.r) * std::cos(t) + c.d * std::cos((c.R - c.r) / c.r * t);
            const double y = (c.R - c.r) * std::sin(t) - c.d * std::sin((c.R - c.r) / c.r * t);
            const double xr = x * std::cos(c.turn) - y * std::sin(c.turn);
            const double yr = x * std::sin(c.turn) + y * std::cos(c.turn);
            pts.push_back({ cx + static_cast<int>(std::lround(xr * scale)), cy + static_cast<int>(std::lround(yr * scale)) });
        }
        pen(g, c.pen);
        poly(g, pts);
    }
    pen(g, 1);
    label(g, 400, 600, 0.35, 0.5, "SPIROGRAPH");
    g += "PU;SP0;";
    return g;
}

namespace {
constexpr int kFox = 1200, kFoy = 3879, kFx1 = 9800, kFyTop = 6900, kFyBot = 860, kFamp = 2800;
}

std::string hpglFunctionAxes() {
    std::string g = "IN;";
    pen(g, 1);
    // From the origin out, a short piece at a time: up, down, right
    auto piecewise = [&g](int x0, int y0, int x1, int y1, int pieces) {
        pu(g, x0, y0);
        for (int i = 1; i <= pieces; ++i) pd(g, x0 + (x1 - x0) * i / pieces, y0 + (y1 - y0) * i / pieces);
    };
    piecewise(kFox, kFoy, kFox, kFyTop, 10);
    piecewise(kFox, kFoy, kFox, kFyBot, 10);
    piecewise(kFox, kFoy, kFx1, kFoy, 24);
    g += "PU;";
    return g;
}

std::string hpglFunctionCurves() {
    std::string g;
    pen(g, 1);
    // Ticks and numbers
    g += "TL1.5,1.5;";
    for (int i = 0; i <= 8; ++i) {
        const int x = kFox + (kFx1 - kFox) * i / 8;
        pu(g, x, kFoy);
        g += "XT;";
        if (i > 0 && i % 2 == 0) label(g, x - 160, kFoy - 330, 0.25, 0.35, std::to_string(i * 2) + "pi");
    }
    for (int i = -2; i <= 2; ++i) {
        const int y = kFoy + kFamp * i / 2;
        pu(g, kFox, y);
        g += "YT;";
        if (i != 0) {
            char b[8];
            std::snprintf(b, sizeof(b), "%.1f", i / 2.0);
            label(g, kFox - 600, y - 70, 0.25, 0.35, b);
        }
    }
    // sin(x)/x and a damped cosine
    std::vector<std::pair<int, int>> a, b;
    for (int i = 0; i <= 320; ++i) {
        const double x = 16 * kPi * i / 320.0;
        const double sx = i == 0 ? 1.0 : std::sin(x) / x;
        const int px = kFox + (kFx1 - kFox) * i / 320;
        a.push_back({ px, kFoy + static_cast<int>(std::lround(kFamp * sx)) });
        b.push_back({ px, kFoy + static_cast<int>(std::lround(kFamp * 0.9 * std::exp(-x / 18) * std::cos(x))) });
    }
    pen(g, 2);
    poly(g, a);
    pen(g, 3);
    poly(g, b);
    pen(g, 2);
    label(g, 5200, 6500, 0.3, 0.45, "SIN(X)/X");
    pen(g, 3);
    label(g, 5200, 6050, 0.3, 0.45, "0.9 EXP(-X/18) COS(X)");
    pen(g, 1);
    label(g, 1300, 7050, 0.4, 0.6, "FUNCTION PLOT");
    g += "PU;SP0;";
    return g;
}

std::string hpglFunctionPlot() { return hpglFunctionAxes() + hpglFunctionCurves(); }

std::string hpglDigitizedMark(int x, int y) {
    std::string g;
    pen(g, 4);
    pu(g, x, y);
    g += "CI120;";
    char b[40];
    std::snprintf(b, sizeof(b), "DP %d,%d", x, y);
    label(g, x + 200, y - 60, 0.25, 0.35, b);
    g += "PU;SP0;";
    return g;
}

std::string hpglLoop() {
    std::string g = "IN;";
    const int cx = 5250, cy = 3879, rx = 3700, ry = 2500;
    // The loop: an ellipse with arrows, the controller on the left
    pen(g, 3);
    std::vector<std::pair<int, int>> pts;
    for (int i = 0; i <= 72; ++i) {
        const double a = 2 * kPi * i / 72;
        pts.push_back({ cx + static_cast<int>(rx * std::cos(a)), cy + static_cast<int>(ry * std::sin(a)) });
    }
    poly(g, pts);
    // Arrows along it (clockwise, as the frames go round)
    for (int i = 0; i < 8; ++i) {
        const double a = 2 * kPi * (i + 0.5) / 8;
        const int x = cx + static_cast<int>(rx * std::cos(a)), y = cy + static_cast<int>(ry * std::sin(a));
        const double tx = rx * std::sin(a), ty = -ry * std::cos(a);       // clockwise tangent
        const double len = std::sqrt(tx * tx + ty * ty);
        const double ux = tx / len, uy = ty / len;
        const int s = 220;
        poly(g, { { x - static_cast<int>(s * (ux - uy * 0.6)), y - static_cast<int>(s * (uy + ux * 0.6)) },
                  { x, y },
                  { x - static_cast<int>(s * (ux + uy * 0.6)), y - static_cast<int>(s * (uy - ux * 0.6)) } });
    }
    // Boxes on the loop: the controller and HIPI's devices
    struct Dev { double a; const char* name; const char* model; int pen; } devs[] = {
        { kPi, "HP-41CX", "controller", 1 },
        { kPi * 0.62, "TFDISPLAY", "HP 82163", 2 },
        { kPi * 0.38, "TFDRIVE", "HP 82161A", 2 },
        { kPi * 0.0, "TFPLOT", "HP 7470A", 4 },
        { -kPi * 0.38, "TFPRINT", "HP 82162A", 4 },
        { -kPi * 0.62, "PILBOX", "PC link", 5 },
    };
    for (const Dev& d : devs) {
        const int x = cx + static_cast<int>(rx * std::cos(d.a)), y = cy + static_cast<int>(ry * std::sin(d.a));
        const int w = 1750, h = 640;
        pen(g, d.pen);
        box(g, x - w / 2, y - h / 2, x + w / 2, y + h / 2);
        box(g, x - w / 2 + 40, y - h / 2 + 40, x + w / 2 - 40, y + h / 2 - 40);
        label(g, x - w / 2 + 120, y + 40, 0.24, 0.38, d.name);
        label(g, x - w / 2 + 120, y - 210, 0.2, 0.3, d.model);
    }
    // In the middle
    pen(g, 1);
    label(g, cx - 1200, cy + 200, 0.6, 0.9, "HP-IL");
    pen(g, 3);
    label(g, cx - 1450, cy - 450, 0.28, 0.42, "frames go round the loop");
    pen(g, 2);
    circle(g, cx, cy, 2000, 64);
    g += "PU;SP0;";
    return g;
}

// ── A made-up HP-IL loop ────────────────────────────────────────────────

namespace {

struct SimDev {
    SimDev(const char* i, std::uint8_t a, const char* d) : id(i), aid(a), data(d) {}
    const char* id;
    std::uint8_t aid;
    const char* data;          // what it sends for SDA
    int addr = 0;
    bool talker = false;
    std::string out;           // being sent (one byte per round)
    std::size_t pos = 0;
};
// (Accessory IDs: the class in the high nibble -- 1x mass storage, 2x
// printer, 3x display, 4x interface, 5x instrument, 6x graphics)
SimDev simPc_[] = { { "HP9114B", 0x10, "" }, { "HP2225B", 0x20, "" }, { "PYILPER", 0x30, "" } };
SimDev simAfter_[] = { { "HP3468A", 0x52, "+1.23456E+0\r\n" }, { "HP82164A", 0x40, "" },
                       { "HP7470A", 0x60, "" } };

IL_CMD_t simDevice(SimDev& d, IL_CMD_t f) {
    if (f == AAU || f == IFC) { if (f == AAU) d.addr = 0; d.talker = false; d.out.clear(); return f; }
    if (f > AAD && f < AAD + 31) {
        if (d.addr != 0) return f;
        d.addr = static_cast<int>(f - AAD);
        return static_cast<IL_CMD_t>(f + 1);
    }
    if (f >= TAD && f < TAD + 31) { d.talker = static_cast<int>(f - TAD) == d.addr; d.out.clear(); return f; }
    if (f == UNT) { d.talker = false; d.out.clear(); return f; }
    if (!d.talker) return f;
    auto send = [&d](const std::string& what) -> IL_CMD_t {
        d.out = what;
        d.pos = 1;
        return static_cast<IL_CMD_t>(static_cast<std::uint8_t>(what[0]));
    };
    if (f == SAI) return send(std::string(1, static_cast<char>(d.aid)));
    if (f == SDI) return send(std::string(d.id) + "\r\n");
    if (f == SST) return send(std::string(1, '\0'));
    if (f == SDA && d.data[0]) return send(d.data);
    // Our byte came round: the next one, or the end
    if (IS_DATA(f) && !d.out.empty()) {
        if (d.pos < d.out.size()) return static_cast<IL_CMD_t>(static_cast<std::uint8_t>(d.out[d.pos++]));
        d.out.clear();
        return ETO;
    }
    return f;
}

unsigned short simPcDevices(unsigned short f) {
    for (SimDev& d : simPc_) f = simDevice(d, f);
    return f;
}

}  // namespace

unsigned short fakeLoopRound(unsigned short f, HipiRound hipi) {
    f = hipi(f, simPcDevices);
    if (f == IL_NO_FRAME) return f;
    for (SimDev& d : simAfter_) f = simDevice(d, f);
    return f;
}

std::vector<LoopAct> fakeSessionActs(int n, int displayAddr, int printerAddr) {
    std::vector<LoopAct> acts;
    auto ask = [&acts](int a, IL_CMD_t what) {
        acts.push_back({ static_cast<IL_CMD_t>(TAD + a), false });
        acts.push_back({ what, true });
        acts.push_back({ UNT, false });
    };
    auto text = [&acts](int a, const char* t) {
        acts.push_back({ static_cast<IL_CMD_t>(LAD + a), false });
        for (const char* p = t; *p; ++p) acts.push_back({ static_cast<IL_CMD_t>(static_cast<std::uint8_t>(*p)), false });
        acts.push_back({ UNL, false });
    };
    for (int a = 1; a <= n && a <= 30; ++a) { ask(a, SAI); ask(a, SDI); }
    if (displayAddr > 0) {
        text(displayAddr, "HELLO FROM THE HP-41\r\n");
        text(displayAddr, "LOOP: HIPI, PILBOX, 3 MORE\r\n");
    }
    if (printerAddr > 0) text(printerAddr, "HP-IL DEMO\r");
    for (int a = 1; a <= n && a <= 30; a += 3) ask(a, SST);
    for (const SimDev& d : simAfter_)
        if (d.data[0] && d.addr > 0) ask(d.addr, SDA);
    return acts;
}

// ── Printer ─────────────────────────────────────────────────────────────

std::vector<std::string> printLines() {
    std::vector<std::string> v;
    v.push_back("\x1b|" "\xD4" "HIPI DEMO" "\r" "\xD0");      // 8-bit mode; double wide
    v.push_back("\xD1" "HP 82162A PRINTER" "\r" "\xD0");      // lower case
    v.push_back(" \r");                                      // (an empty line: a space)
    // A program listing, as PRP prints it
    for (const char* l : { "01*LBL \"SINTAB\"", "02 DEG", "03 FIX 4", "04 0", "05 STO 00",
                           "06*LBL 01", "07 RCL 00", "08 SIN", "09 RCL 00", "10 ACX", "11 X<>Y",
                           "12 ACX", "13 PRBUF", "14 15", "15 ST+ 00", "16 90", "17 RCL 00",
                           "18 X<=Y?", "19 GTO 01", "20 END" })
        v.push_back(std::string(l) + "\r");
    v.push_back(" \r");
    // ... and what it prints: a table, numbers right justified
    v.push_back("\xD4" "SIN TABLE" "\r" "\xD0");
    for (int deg = 0; deg <= 90; deg += 15) {
        char b[32];
        std::snprintf(b, sizeof(b), "%2d%22.4f\r", deg, std::sin(deg * kPi / 180.0));
        v.push_back(b);
    }
    v.push_back(" \r");
    v.push_back("\xE8" "3.1416  ***\r" "\xE0");               // right justified (as PRX)
    std::string hp;
    for (int c = 0; c < 24; ++c) hp += static_cast<char>(c == 13 ? 'x' : c);   // the HP-41's own characters
    v.push_back(hp + "\r");
    std::string gr = "\xD2";                                   // column mode: a sine wave
    for (int x = 0; x < 168; ++x) {
        const double y = 3.5 + 3.4 * std::sin(x * 2 * kPi / 56.0);
        gr += static_cast<char>(1 << static_cast<int>(y + 0.5));
    }
    v.push_back(gr + "\xD0" "\r");
    // A bar chart three lines tall (21 dots), a column a dot wide, each bar
    // under its year (3 characters = 21 columns). Column bytes stay below
    // 0x80 -- higher ones are the printer's escape codes.
    v.push_back(" \r");
    v.push_back("SALES 1981-87\r");
    const int heights[] = { 6, 9, 14, 11, 16, 19, 21 };
    for (int line = 0; line < 3; ++line) {
        std::string bars = "\xD2";
        for (int col = 0; col < 168; ++col) {
            const int b = col / 21;
            const int h = b < 7 && col % 21 < 16 ? heights[b] : 0;
            int byte = 0;
            for (int dot = 0; dot < 7; ++dot)
                if (line * 7 + dot >= 21 - h) byte |= 1 << dot;
            bars += static_cast<char>(byte);
        }
        v.push_back(bars + "\xD0" "\r");
    }
    v.push_back("81 82 83 84 85 86 87\r");
    v.push_back(" \r");
    v.push_back("\xD4" "THE END" "\r" "\xD0");
    return v;
}

}  // namespace demo
}  // namespace hipi
