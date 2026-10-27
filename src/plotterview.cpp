// plotterview.cpp -- the Plotter view (views.h): the plot drawn from
// CPlotter's segments, Page or Fit, the pens and the paper colour, the
// ERROR light and the digitize panel (HP-GL DP). The rest of the views,
// and switching between them, is views.cpp.
#include "views.h"
#include "views_impl.h"
#include "boardui.h"
#include "usb_serial.h"
#include "backlight.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include "pico/time.h"

namespace hipi {

using namespace views_impl;

namespace {

std::int16_t i16(long v) { return static_cast<std::int16_t>(v); }

CPlotter* plotter_ = nullptr;

// Page mapping from plotter units to screen pixels -- fits the plotter's
// whole area (0,0 .. 10300,7650: the paper -- text may start left of the
// factory P1 at 250,279) into the panel, keeping the aspect ratio
// (letterboxed). IP only moves where user units (SC) land on that paper.
constexpr double kP1X = 0.0, kP1Y = 0.0, kP2X = CPlotter::kMaxX, kP2Y = CPlotter::kMaxY;
constexpr double kPlotW = kP2X - kP1X;   // 10300
constexpr double kPlotH = kP2Y - kP1Y;   // 7650
constexpr double kScaleX = static_cast<double>(SCREEN_MAX_X) / kPlotW;
constexpr double kScaleY = static_cast<double>(SCREEN_MAX_Y) / kPlotH;
constexpr double kScale = (kScaleX < kScaleY) ? kScaleX : kScaleY;
constexpr double kFitW = kPlotW * kScale;
constexpr double kFitH = kPlotH * kScale;
constexpr double kOffsetX = (SCREEN_MAX_X - kFitW) / 2.0;
constexpr double kOffsetY = (SCREEN_MAX_Y - kFitH) / 2.0;

// The mapping in use. Two ways to show the plot (Display -> Plot view):
//  Page -- the plotter's whole area (10300 x 7650 units) fitted to the screen,
//          centred: what the paper would show, but the HP-41's plots don't
//          sit in the middle of it (room for the axis labels, left/bottom).
//  Fit  -- the plot itself (the bounding box of everything drawn, plus a
//          margin) fitted to the screen and centred, so it uses the screen
//          best. Grows (with some slack) as the plot grows.
// Both keep the aspect ratio -- circles stay round, text undistorted.
// The screen width the plot gets: all of it, but while digitizing (DP)
// the panel on the right takes its part (see "Digitizing" below)
int plotAreaW_ = SCREEN_MAX_X;
double mapOX_ = kP1X, mapOY_ = kP1Y;      // plotter units at the content's left / bottom
double mapScale_ = kScale;                // pixels per plotter unit
double mapDX_ = kOffsetX, mapDY_ = kOffsetY, mapH_ = kFitH;
double fitX0_ = 0, fitY0_ = 0, fitX1_ = -1, fitY1_ = -1;   // Fit: the area shown (units)

void setPageMapping() {
    mapOX_ = kP1X; mapOY_ = kP1Y; mapScale_ = kScale;
    mapDX_ = kOffsetX; mapDY_ = kOffsetY; mapH_ = kFitH;
    if (plotAreaW_ < SCREEN_MAX_X) {          // (narrower: the page fitted to what's left)
        mapScale_ = std::min(plotAreaW_ / kPlotW, SCREEN_MAX_Y / kPlotH);
        mapH_ = kPlotH * mapScale_;
        mapDX_ = (plotAreaW_ - kPlotW * mapScale_) / 2;
        mapDY_ = (SCREEN_MAX_Y - mapH_) / 2;
    }
    fitX0_ = 0; fitY0_ = 0; fitX1_ = -1; fitY1_ = -1;
}

// Shows the plotter-unit area x0..x1, y0..y1 (plus `margin` of its size
// on each side) as large as fits, centred
void setFitMapping(double x0, double y0, double x1, double y1, double margin) {
    double w = std::max(x1 - x0, 200.0), h = std::max(y1 - y0, 200.0);
    const double cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
    w *= 1 + 2 * margin;
    h *= 1 + 2 * margin;
    fitX0_ = cx - w / 2; fitX1_ = cx + w / 2;
    fitY0_ = cy - h / 2; fitY1_ = cy + h / 2;
    double s = std::min(plotAreaW_ / w, SCREEN_MAX_Y / h);
    s = std::min(s, kScale * 4);                       // a tiny plot: not more than 4x the page
    mapScale_ = s;
    mapOX_ = fitX0_;
    mapOY_ = fitY0_;
    mapH_ = h * s;
    mapDX_ = (plotAreaW_ - w * s) / 2;
    mapDY_ = (SCREEN_MAX_Y - mapH_) / 2;
}

bool plotFit_ = true;                     // Display -> Plot view (config plot_fit)
double fitMargin_ = 0.05;                 // Fit: room around the plot (more while it grows)

std::int16_t mapX(std::int16_t px) {
    // round(), not a truncating cast -- at our scale (the whole 10000x7200
    // unit P1-P2 page squeezed into about 1024x600 pixels, ~12 units/pixel), thin
    // details like a minus sign or a comma's tail are only 1-2 pixels to
    // begin with, so truncation's systematic downward bias makes it easy
    // to lose them entirely.
    return static_cast<std::int16_t>(std::lround(mapDX_ + (static_cast<double>(px) - mapOX_) * mapScale_));
}

std::int16_t mapY(std::int16_t py) {
    // Flip vertical: plotter Y grows "up" (away from the origin), pixel Y
    // grows down the screen. Same rounding note as mapX() above.
    return static_cast<std::int16_t>(std::lround(mapDY_ + mapH_ - (static_cast<double>(py) - mapOY_) * mapScale_));
}

// The plot is drawn like on real plotter paper: dark pens on white
// (default), or inverted -- light pens on black (Plotter -> Paper,
// config plot_paper=white|black).
bool paperBlack_ = false;

std::uint16_t paperColor() { return paperBlack_ ? 0x0000 : 0xFFFF; }

// Pen colours (RGB565) for SP 1, 2, 3, ... -- HP-GL only selects a pen
// NUMBER; which colour sits in that pen stall is up to the user, so this
// table is the "pen carousel". Pens beyond the table cycle through it.
// Two sets: tones that show up well on white paper (a plain yellow
// wouldn't), and brighter tones for black paper (pen 1 becomes white).
constexpr std::uint16_t kPenColorsWhite[] = {
    0x0000,   // 1: black
    0xE000,   // 2: red
    0x001A,   // 3: blue
    0x0460,   // 4: green
    0xCD00,   // 5: yellow (dark gold, readable on white)
};
constexpr std::uint16_t kPenColorsBlack[] = {
    0xFFFF,   // 1: white
    0xFA08,   // 2: red (slightly light, readable on black)
    0x4C1F,   // 3: blue (light, plain blue is too dark on black)
    0x07E8,   // 4: green
    0xFFE0,   // 5: yellow
};
constexpr int kPenColorCount = static_cast<int>(sizeof(kPenColorsWhite) / sizeof(kPenColorsWhite[0]));
static_assert(sizeof(kPenColorsBlack) == sizeof(kPenColorsWhite), "pen tables must match");

std::uint16_t penColor(std::uint8_t pen) {
    const std::uint16_t* pens = paperBlack_ ? kPenColorsBlack : kPenColorsWhite;
    // Pen 0 = no pen selected (after IN, before any SP): draw with pen 1
    // anyway, the most useful default for programs that never select one
    if (pen == 0) return pens[0];
    return pens[(pen - 1) % kPenColorCount];
}

// Draws one plotter-space segment, mapped to screen pixels. If the
// mapping rounds both endpoints to the *same* screen pixel (very common
// at our scale -- roughly 15 plotter units per pixel -- for the tightly
// curved parts of glyphs like the round strokes in "6", "8", "2"), a
// straight line() call can draw nothing at all: many line-drawing engines
// (including the LT7683's own hardware one) treat identical start/end
// points as "zero pixels to step through" rather than drawing a single
// dot. Since the *plotter-space* segment is real and non-degenerate, it
// must still show up as something -- so fall back to a single pixel
// instead of silently losing it.
//
// The pen's tip (Plotter -> Pen, kept per segment): thin is one pixel;
// wide is kWidePenPx across, a round tip -- drawn as the 1-pixel line (or
// dot) once for every pixel of the tip (offsets within its radius), so
// ends and joins come out round.
constexpr int kWidePenPx = 3;
void drawMappedSegment(std::int16_t x0, std::int16_t y0,
                       std::int16_t x1, std::int16_t y1, std::uint16_t color, bool wide) {
    const std::int16_t sx0 = mapX(x0), sy0 = mapY(y0);
    const std::int16_t sy1 = mapY(y1);
    const std::int16_t sx1 = mapX(x1);
    const int r = wide ? kWidePenPx / 2 : 0;
    for (int dy = -r; dy <= r; ++dy)
        for (int dx = -r; dx <= r; ++dx) {
            if (dx * dx + dy * dy > r * r) continue;          // (a round tip)
            const auto ax = static_cast<std::int16_t>(sx0 + dx), ay = static_cast<std::int16_t>(sy0 + dy);
            const auto bx = static_cast<std::int16_t>(sx1 + dx), by = static_cast<std::int16_t>(sy1 + dy);
            if (ax == bx && ay == by) display_->pixel(ax, ay, color);
            else display_->line(ax, ay, bx, by, color);
        }
}

// The plot always covers the whole panel. On the 7" panel the button strip
// is a PIP overlay, so the panel underneath belongs to the plot too -- but
// showing the strip (or updating a LED on it) leaves the active window
// narrowed to the area left of it, which would clip the plot there: fills
// and lines under the strip were left out, and showed once the strip went
// away (e.g. still the old paper colour after a Paper change). So widen it
// before drawing.
void plotWindow() {
    if (plotAreaW_ < SCREEN_MAX_X) {          // digitizing: not over the panel
        display_->setActiveWindow(0, 0, static_cast<std::uint16_t>(plotAreaW_ - 1), SCREEN_MAX_Y - 1);
        return;
    }
    display_->setActiveWindow(0, 0, SCREEN_MAX_X - 1, SCREEN_MAX_Y - 1);
}

// ── The ERROR light ─────────────────────────────────────────────────────
// The 7470A's front-panel ERROR LED: lit while the plotter has an HP-GL
// error (CPlotter::errorCode() -- OE, IN or Plotter -> Errors -> Clear
// error put it out). Drawn on the plot, top centre, in a small dark box
// with the error number; a touch on it says what the error was.
constexpr int kErrW = 112, kErrH = 32, kErrY = 8;
bool errLit_ = false;                     // drawn now
std::uint32_t errSerial_ = 0;             // CPlotter::errorSerial() when drawn
int errX() { return ((plotAreaW_ - kErrW) / 2) & ~3; }
void drawErrorLight() {
    errLit_ = false;
    if (plotter_ == nullptr || plotter_->errorCode() == 0) return;
    const int x = errX(), y = kErrY;
    display_->fillRoundRect(static_cast<std::int16_t>(x), y, kErrW, kErrH, 8, 0x18E3);
    display_->roundRect(static_cast<std::int16_t>(x), y, kErrW, kErrH, 8, 0x4208);
    display_->fillCircle(static_cast<std::int16_t>(x + 18), y + kErrH / 2, 8, 0xFFE0);   // the LED, lit
    display_->circle(static_cast<std::int16_t>(x + 18), y + kErrH / 2, 9, 0x8400);
    char t[16];
    std::snprintf(t, sizeof(t), "ERROR %d", plotter_->errorCode());
    display_->selectBuiltinFont();
    display_->txtSize(0);
    display_->txtColor(0xFFFF, 0x18E3);
    display_->txtSetCursor(static_cast<std::uint16_t>(x + 34), static_cast<std::uint16_t>(y + (kErrH - 16) / 2));
    display_->txtWrite(t);
    screen_->reassertRenderState();
    errLit_ = true;
}
// A segment drawn (screen box) over the light: draw it again on top
bool overErrorLight(int x0, int y0, int x1, int y1) {
    const int lx = errX();
    return errLit_ && std::max(x0, x1) + 2 >= lx && std::min(x0, x1) - 2 < lx + kErrW &&
           std::max(y0, y1) + 2 >= kErrY && std::min(y0, y1) - 2 < kErrY + kErrH;
}

void drawDigitizeMarker();                  // (Digitizing, below)
void drawDigitizePanel();

// Live per-segment draw, called from CPlotter's onDraw_ callback as HP-GL
// commands stream in. No-ops entirely (segments() still records it for a
// later redraw) unless Plotter output is what's actually showing right now.
void onPlotterDraw(std::int16_t x0, std::int16_t y0,
                   std::int16_t x1, std::int16_t y1, std::uint8_t pen) {
    backlight_activity();        // new drawing: screen in use (backlight.h)
    if (output_ != DisplayOutput::Plotter) return;
    // Fit: a segment outside what's shown -- fit again, with slack so it
    // doesn't happen at every step while the plot grows, and redraw
    if (plotFit_ && (fitX1_ < fitX0_ ||
                     std::min(x0, x1) < fitX0_ || std::max(x0, x1) > fitX1_ ||
                     std::min(y0, y1) < fitY0_ || std::max(y0, y1) > fitY1_)) {
        {
            // More room than a fresh fit (15 %): a plot that grows a little
            // at a time (axes drawn in pieces) is redrawn every few pieces,
            // not at each one -- every redraw is the whole plot again, also
            // to hipiview
            fitMargin_ = 0.15;
            views_redraw();                 // (refits to all segments, this one included)
            fitMargin_ = 0.05;
            return;
        }
    }
    plotWindow();
    drawMappedSegment(x0, y0, x1, y1, penColor(pen), plotter_->wideTip());   // (this segment's tip)
    if (overErrorLight(mapX(x0), mapY(y0), mapX(x1), mapY(y1))) drawErrorLight();   // (kept on top)
    if (plotAreaW_ < SCREEN_MAX_X) drawDigitizeMarker();   // (kept on top)
    // line()/pixel() go through gfxMode(), which blindly zeros MWCR0 (the
    // same text-mode/cursor-visible register Screen owns) -- same fix
    // pattern used throughout boardui.cpp for its own
    // display_->drawBitmap565()/fillRect() calls.
    screen_->refreshCursor();
}

// Fired when the plotter is reset (IN command, or a real HP-IL Clear
// Device). Only touches the screen if Plotter output is actually showing.
void onPlotterClear() {
    // Forget the old plot's Fit area: else a new, smaller plot inside it
    // would be drawn at the old scale, never fitted (the first segment now
    // fits afresh -- see onPlotterDraw())
    setPageMapping();
    if (output_ != DisplayOutput::Plotter) return;
    plotWindow();
    display_->fillRect(0, 0, SCREEN_MAX_X, SCREEN_MAX_Y, paperColor());
    if (plotAreaW_ < SCREEN_MAX_X) drawDigitizePanel();
    drawErrorLight();                         // (IN puts it out -- still lit after a DCL with an error)
    screen_->refreshCursor();
}

// ── Digitizing (HP-GL DP) ───────────────────────────────────────────────
// A panel on the right, like the plotter's own front panel: a loupe (4x the
// plot around the marker), X/Y in plotter units (and user units under SC),
// four arrows that move the marker one pixel, ENTER and X (cancel). A touch
// on the plot puts the marker there; the point is only given on ENTER. The
// panel sits left of the button strip's zone (the strip keeps its place);
// the plot is fitted into what's left (plotAreaW_). The marker -- and the
// loupe's cross -- are drawn twice (an outline in the paper's opposite
// colour, then magenta inside), so they show on white and black paper alike.
constexpr int kDigiPanelW = 240, kDigiCell = 56, kDigiBtnH = 46;
constexpr int kDigiPad = 12, kDigiMag = 4, kDigiArm = 12, kDigiGap = 4;
constexpr std::uint16_t kDigiBg = 0x2104, kDigiYellow = 0xEDC0, kDigiMagenta = 0xF81F,
                        kDigiBtn = 0x39E7, kDigiEdge = 0xC618, kDigiGreen = 0x0467, kDigiRed = 0x9104;

struct DigiRect { int x, y, w, h; bool hit(int px, int py) const { return px >= x - 4 && px < x + w + 4 && py >= y - 4 && py < y + h + 4; } };
struct DigiLayout {
    int x0 = 0;                               // the panel's left edge (= plotAreaW_)
    DigiRect loupe, coords, up, down, left, right, enter, cancel;
};
DigiLayout digi_;
bool digiActive_ = false;                     // the panel is up (plotter waiting for a point)
int digiMX_ = 0, digiMY_ = 0;                 // the marker, screen pixels
int digiRepeatDx_ = 0, digiRepeatDy_ = 0;     // an arrow held down
absolute_time_t digiRepeatNext_ = nil_time;

void digiLayout() {
    const int stripW = boardui_buttonStripWidth();
    DigiLayout& L = digi_;
    L.x0 = SCREEN_MAX_X - stripW - kDigiPanelW;
    const int s = kDigiCell;
    // Top to bottom: title, loupe, X/Y, the arrows, ENTER / X
    int lsz = SCREEN_MAX_Y - (44 + 8 + 88 + 4 + 3 * s + 8 + kDigiBtnH + 8 + 4);
    lsz = std::min(lsz, kDigiPanelW - 2 * kDigiPad);
    L.loupe = { L.x0 + (kDigiPanelW - lsz) / 2, 44, lsz, lsz };
    L.coords = { L.x0 + kDigiPad, L.loupe.y + lsz + 8, kDigiPanelW - 2 * kDigiPad, 88 };
    const int cx = L.x0 + kDigiPanelW / 2, top = L.coords.y + 88 + 4;
    L.up    = { cx - s / 2, top, s, s };
    L.left  = { cx - s / 2 - s - 4, top + s, s, s };
    L.right = { cx + s / 2 + 4, top + s, s, s };
    L.down  = { cx - s / 2, top + 2 * s, s, s };
    const int by = SCREEN_MAX_Y - kDigiBtnH - 8, ew = (kDigiPanelW - 3 * kDigiPad) * 62 / 100;
    L.enter  = { L.x0 + kDigiPad, by, ew, kDigiBtnH };
    L.cancel = { L.x0 + 2 * kDigiPad + ew, by, kDigiPanelW - 3 * kDigiPad - ew, kDigiBtnH };
}

// The marker's point in plotter units (what ENTER gives)
void digiPoint(double& px, double& py) {
    px = mapOX_ + (digiMX_ - mapDX_) / mapScale_;
    py = mapOY_ + (mapDY_ + mapH_ - digiMY_) / mapScale_;
    px = std::max(0.0, std::min(px, static_cast<double>(CPlotter::kMaxX)));
    py = std::max(0.0, std::min(py, static_cast<double>(CPlotter::kMaxY)));
}

void digiText(int x, int y, const char* t, std::uint8_t size, std::uint16_t fg, std::uint16_t bg) {
    display_->selectBuiltinFont();
    display_->txtSize(size);
    display_->txtColor(fg, bg);
    display_->txtSetCursor(static_cast<std::uint16_t>(x), static_cast<std::uint16_t>(y));
    display_->txtWrite(t);
}

// A filled rectangle cut to the screen first: the chips take negative or
// too-large corners as huge unsigned ones (a marker at the plot's edge drew
// magenta lines along the screen's left and bottom edges)
void digiFill(int x, int y, int w, int h, std::uint16_t c) {
    const int x1 = std::min(x + w, static_cast<int>(SCREEN_MAX_X)), y1 = std::min(y + h, static_cast<int>(SCREEN_MAX_Y));
    x = std::max(x, 0);
    y = std::max(y, 0);
    if (x1 > x && y1 > y) display_->fillRect(i16(x), i16(y), i16(x1 - x), i16(y1 - y), c);
}

// A cross with a gap in the middle: arms outlined, magenta inside (bold:
// thicker, for the marker on the plot; the loupe's is thin, it's exact)
void digiCross(int x, int y, int arm, int gap, bool bold) {
    for (int pass = 0; pass < 2; ++pass) {
        const int t = (pass == 0 ? 1 : 0) + (bold ? 1 : 0);     // half thickness
        // (the outline in the paper's opposite: black on white, white on black)
        const std::uint16_t c = pass == 0 ? (paperBlack_ ? 0xFFFF : 0x0000) : kDigiMagenta;
        digiFill(x - gap - arm, y - t, arm, 2 * t + 1, c);
        digiFill(x + gap + 1, y - t, arm, 2 * t + 1, c);
        digiFill(x - t, y - gap - arm, 2 * t + 1, arm, c);
        digiFill(x - t, y + gap + 1, 2 * t + 1, arm, c);
    }
    display_->pixel(i16(x), i16(y), kDigiMagenta);
}

void drawDigitizeMarker() {
    display_->setActiveWindow(0, 0, static_cast<std::uint16_t>(plotAreaW_ - 1), SCREEN_MAX_Y - 1);
    digiCross(digiMX_, digiMY_, kDigiArm, kDigiGap, true);
    display_->setActiveWindow(0, 0, SCREEN_MAX_X - 1, SCREEN_MAX_Y - 1);
}

// Cohen-Sutherland, for the loupe (its lines can be far longer than the screen)
bool digiClip(double& x0, double& y0, double& x1, double& y1, double l, double t, double r, double b) {
    auto code = [&](double x, double y) { return (x < l ? 1 : x > r ? 2 : 0) | (y < t ? 4 : y > b ? 8 : 0); };
    int c0 = code(x0, y0), c1 = code(x1, y1);
    for (int i = 0; i < 8; ++i) {
        if (!(c0 | c1)) return true;
        if (c0 & c1) return false;
        const int c = c0 ? c0 : c1;
        double x, y;
        if (c & 8)      { x = x0 + (x1 - x0) * (b - y0) / (y1 - y0); y = b; }
        else if (c & 4) { x = x0 + (x1 - x0) * (t - y0) / (y1 - y0); y = t; }
        else if (c & 2) { y = y0 + (y1 - y0) * (r - x0) / (x1 - x0); x = r; }
        else            { y = y0 + (y1 - y0) * (l - x0) / (x1 - x0); x = l; }
        if (c == c0) { x0 = x; y0 = y; c0 = code(x0, y0); } else { x1 = x; y1 = y; c1 = code(x1, y1); }
    }
    return false;
}

void drawDigitizeLoupe() {
    const DigiRect& r = digi_.loupe;
    display_->fillRect(i16(r.x), i16(r.y), i16(r.w), i16(r.h), paperColor());
    double mx, my;
    digiPoint(mx, my);
    const double s = mapScale_ * kDigiMag;     // loupe pixels per plotter unit
    const double cx = r.x + r.w / 2.0, cy = r.y + r.h / 2.0;
    const double half = (r.w / 2.0) / s;       // plotter units from the centre to the edge
    display_->setActiveWindow(static_cast<std::uint16_t>(r.x), static_cast<std::uint16_t>(r.y),
                              static_cast<std::uint16_t>(r.x + r.w - 1), static_cast<std::uint16_t>(r.y + r.h - 1));
    for (const PlotSegment& seg : plotter_->segments()) {
        if (std::max(seg.x0, seg.x1) < mx - half || std::min(seg.x0, seg.x1) > mx + half ||
            std::max(seg.y0, seg.y1) < my - half || std::min(seg.y0, seg.y1) > my + half) continue;
        double x0 = cx + (seg.x0 - mx) * s, y0 = cy - (seg.y0 - my) * s;
        double x1 = cx + (seg.x1 - mx) * s, y1 = cy - (seg.y1 - my) * s;
        if (!digiClip(x0, y0, x1, y1, r.x, r.y, r.x + r.w - 1, r.y + r.h - 1)) continue;
        const auto sx0 = i16(std::lround(x0)), sy0 = i16(std::lround(y0));
        const auto sx1 = i16(std::lround(x1)), sy1 = i16(std::lround(y1));
        if (sx0 == sx1 && sy0 == sy1) display_->pixel(sx0, sy0, penColor(seg.pen));
        else display_->line(sx0, sy0, sx1, sy1, penColor(seg.pen));
    }
    digiCross(static_cast<int>(cx), static_cast<int>(cy), r.w / 4, 5, false);
    display_->setActiveWindow(0, 0, SCREEN_MAX_X - 1, SCREEN_MAX_Y - 1);
    display_->rect(i16(r.x - 1), i16(r.y - 1), i16(r.w + 2), i16(r.h + 2), kDigiEdge);
}

void drawDigitizeCoords() {
    const DigiRect& r = digi_.coords;
    display_->fillRect(i16(r.x), i16(r.y), i16(r.w), i16(r.h), kDigiBg);
    double px, py, ux, uy;
    digiPoint(px, py);
    char b[40];
    std::snprintf(b, sizeof(b), "X %5ld", std::lround(px));
    digiText(r.x, r.y, b, 1, 0xFFFF, kDigiBg);
    std::snprintf(b, sizeof(b), "Y %5ld", std::lround(py));
    digiText(r.x, r.y + 34, b, 1, 0xFFFF, kDigiBg);
    if (plotter_->userUnits(px, py, ux, uy)) {
        std::snprintf(b, sizeof(b), "user %.4g, %.4g", ux, uy);
        digiText(r.x, r.y + 70, b, 0, 0xAD55, kDigiBg);
    }
}

void digiButton(const DigiRect& r, std::uint16_t fill, const char* label, char arrow) {
    display_->fillRoundRect(i16(r.x), i16(r.y), i16(r.w), i16(r.h), 8, fill);
    display_->roundRect(i16(r.x), i16(r.y), i16(r.w), i16(r.h), 8, kDigiEdge);
    const int cx = r.x + r.w / 2, cy = r.y + r.h / 2, a = std::min(r.w, r.h) / 4;
    switch (arrow) {
        case 'u': display_->fillTriangle(i16(cx), i16(cy - a), i16(cx - a), i16(cy + a * 7 / 10), i16(cx + a), i16(cy + a * 7 / 10), 0xFFFF); break;
        case 'd': display_->fillTriangle(i16(cx), i16(cy + a), i16(cx - a), i16(cy - a * 7 / 10), i16(cx + a), i16(cy - a * 7 / 10), 0xFFFF); break;
        case 'l': display_->fillTriangle(i16(cx - a), i16(cy), i16(cx + a * 7 / 10), i16(cy - a), i16(cx + a * 7 / 10), i16(cy + a), 0xFFFF); break;
        case 'r': display_->fillTriangle(i16(cx + a), i16(cy), i16(cx - a * 7 / 10), i16(cy - a), i16(cx - a * 7 / 10), i16(cy + a), 0xFFFF); break;
        default: break;
    }
    if (label != nullptr) {
        const int tw = static_cast<int>(std::strlen(label)) * 16;
        digiText(cx - tw / 2, cy - 16, label, 1, 0xFFFF, fill);
    }
}

void drawDigitizePanel() {
    digiLayout();
    display_->setActiveWindow(0, 0, SCREEN_MAX_X - 1, SCREEN_MAX_Y - 1);
    // The panel and the strip's zone to its right (the strip shows over it)
    display_->fillRect(i16(digi_.x0), 0, i16(SCREEN_MAX_X - digi_.x0), SCREEN_MAX_Y, kDigiBg);
    display_->fillRect(i16(digi_.x0), 0, 3, SCREEN_MAX_Y, kDigiYellow);
    digiText(digi_.x0 + kDigiPad, 6, "DIGITIZE", 1, kDigiYellow, kDigiBg);
    drawDigitizeLoupe();
    drawDigitizeCoords();
    digiButton(digi_.up, kDigiBtn, nullptr, 'u');
    digiButton(digi_.down, kDigiBtn, nullptr, 'd');
    digiButton(digi_.left, kDigiBtn, nullptr, 'l');
    digiButton(digi_.right, kDigiBtn, nullptr, 'r');
    digiButton(digi_.enter, kDigiGreen, "ENTER", 0);
    digiButton(digi_.cancel, kDigiRed, "X", 0);
    screen_->reassertRenderState();
    drawDigitizeMarker();
}

// The marker to x,y (screen pixels, kept in the plot area): what was under
// the old one is drawn again, then the marker, the loupe and X/Y
void digiMoveTo(int x, int y) {
    x = std::max(0, std::min(x, plotAreaW_ - 1));
    y = std::max(0, std::min(y, SCREEN_MAX_Y - 1));
    if (x == digiMX_ && y == digiMY_) return;   // (an arrow held at the edge: nothing to draw)
    const int r = kDigiArm + kDigiGap + 2;
    const int ex0 = std::max(0, digiMX_ - r), ey0 = std::max(0, digiMY_ - r);
    const int ex1 = std::min(plotAreaW_, digiMX_ + r + 1), ey1 = std::min(SCREEN_MAX_Y, digiMY_ + r + 1);
    digiMX_ = x;
    digiMY_ = y;
    views_redrawRegion(i16(ex0), i16(ey0), i16(ex1 - ex0), i16(ey1 - ey0));
    drawDigitizeMarker();
    drawDigitizeLoupe();
    drawDigitizeCoords();
    screen_->reassertRenderState();
    screen_->refreshCursor();
}

}  // namespace

void views_impl::plotInit(CPlotter* plotter) {
    plotter_ = plotter;
    plotter_->setDrawCallback(onPlotterDraw);
    plotter_->setClearCallback(onPlotterClear);
}

void views_impl::plotRedraw() {
    // The mapping: the whole page, or (Fit) the plot itself with a margin
    // -- 5 % when fitted afresh here, so it has room to grow a little
    // (15 % when it grew out of what was shown: fitMargin_)
    setPageMapping();
    if (plotFit_ && !plotter_->segments().empty()) {
        double x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
        for (const PlotSegment& seg : plotter_->segments()) {
            x0 = std::min(x0, static_cast<double>(std::min(seg.x0, seg.x1)));
            x1 = std::max(x1, static_cast<double>(std::max(seg.x0, seg.x1)));
            y0 = std::min(y0, static_cast<double>(std::min(seg.y0, seg.y1)));
            y1 = std::max(y1, static_cast<double>(std::max(seg.y0, seg.y1)));
        }
        setFitMapping(x0, y0, x1, y1, fitMargin_);
    }
    plotWindow();
    display_->fillRect(0, 0, SCREEN_MAX_X, SCREEN_MAX_Y, paperColor());
    for (const PlotSegment& seg : plotter_->segments()) {
        drawMappedSegment(seg.x0, seg.y0, seg.x1, seg.y1, penColor(seg.pen), seg.wide);
    }
    drawErrorLight();
    errSerial_ = plotter_->errorSerial();
    if (plotAreaW_ < SCREEN_MAX_X) drawDigitizePanel();      // (and the marker)
    screen_->refreshCursor();
}

void views_impl::plotRedrawRegion(std::int16_t x0, std::int16_t y0, std::int16_t w, std::int16_t h) {
    // Narrow the active window to just this region -- replaying every
    // segment below then only actually draws the parts that fall inside
    // it (the LT7683 clips the rest away in hardware), avoiding the
    // need to do our own segment/rectangle intersection math for the
    // PIXELS actually drawn.
    display_->setActiveWindow(static_cast<std::uint16_t>(x0), static_cast<std::uint16_t>(y0),
                              static_cast<std::uint16_t>(x0 + w - 1),
                              static_cast<std::uint16_t>(y0 + h - 1));
    display_->fillRect(x0, y0, w, h, paperColor());
    // BTE (memory-copy) doesn't apply here: this region's own pixels
    // were never drawn in the first place (the content area is
    // NARROWER than the full panel for as long as the button strip is
    // up -- see boardui.cpp's showButtonStrip()), so there's no prior
    // rendered state anywhere in SDRAM to copy from, only source segment
    // data to compute fresh from. showButtonStrip()/hideButtonStrip()
    // already DO use BTE for their own part of this (sliding the
    // existing, already-rendered pixels on EITHER side of this region
    // out of/back into the way -- pure hardware memory moves, content-
    // agnostic, so they work identically whether Display or Plotter
    // output is underneath) -- this function only ever needs to fill
    // the one sliver those shifts can't fill by themselves.
    //
    // What CAN help here, short of an actual memory-copy: skip the
    // hardware line-draw engine call entirely for any segment whose own
    // screen-space bounding box doesn't even overlap this region at all
    // -- the LT7683's own hardware clipping (via the active window set
    // above) would have discarded it anyway, but only AFTER the
    // DLHSR0-DLVER0 coordinate registers and DCR0 trigger were all
    // still written over SPI first. For a plot with many segments and a
    // narrow target region (exactly this call's own use case -- the
    // vacated button-strip sliver), most segments fall outside it
    // entirely, so this check trades a handful of cheap integer
    // comparisons for what would otherwise be a full, wasted SPI
    // register-write sequence per skipped segment.
    const std::int16_t regionRight = static_cast<std::int16_t>(x0 + w);
    const std::int16_t regionBottom = static_cast<std::int16_t>(y0 + h);
    for (const PlotSegment& seg : plotter_->segments()) {
        const std::int16_t sx0 = mapX(seg.x0), sy0 = mapY(seg.y0);
        const std::int16_t sx1 = mapX(seg.x1), sy1 = mapY(seg.y1);
        const std::int16_t segLeft = std::min(sx0, sx1);
        const std::int16_t segRight = std::max(sx0, sx1);
        const std::int16_t segTop = std::min(sy0, sy1);
        const std::int16_t segBottom = std::max(sy0, sy1);
        if (segRight < x0 || segLeft > regionRight ||
            segBottom < y0 || segTop > regionBottom) {
            continue;  // bounding box entirely outside the region -- skip
        }
        drawMappedSegment(seg.x0, seg.y0, seg.x1, seg.y1, penColor(seg.pen), seg.wide);
    }
    // Restore the full-screen active window Plotter mode normally runs
    // with -- the caller (boardui.cpp's hideButtonStrip()) already leaves
    // it wide at this point, so this just keeps it that way.
    display_->setActiveWindow(0, 0, SCREEN_MAX_X - 1, SCREEN_MAX_Y - 1);
    screen_->refreshCursor();
}

bool views_errorLightTap(int x, int y) {
    if (output_ != DisplayOutput::Plotter || !errLit_ || plotter_ == nullptr) return false;
    if (x < errX() || x >= errX() + kErrW || y < kErrY || y >= kErrY + kErrH) return false;
    views_showMessage(views_errorText().c_str());
    return true;
}

bool views_errorLightPoint(int& x, int& y) {
    if (output_ != DisplayOutput::Plotter || !errLit_) return false;
    x = errX() + 18;
    y = kErrY + kErrH / 2;
    return true;
}

std::string views_errorText() {
    if (plotter_ == nullptr || plotter_->errorCode() == 0) return "No error";
    const int code = plotter_->errorCode();
    std::string t = "Error " + std::to_string(code) + ": " + CPlotter::errorText(code);
    const auto& log = plotter_->errorLog();
    if (!log.empty() && log.back().code == code && log.back().command[0] != 0)
        t += " -- " + std::string(log.back().command);
    return t;
}

// DP: the Plotter view comes up with the panel (see "Digitizing" above);
// when the plotter stops waiting (ENTER, X, DC, IN) the panel goes
static void pollDigitize() {
    const bool waiting = plotter_ != nullptr && plotter_->digitizing();
    if (waiting && !digiActive_) {
        digiActive_ = true;
        backlight_activity();                // (a screen saver ends: the touch is wanted)
        digiLayout();
        plotAreaW_ = digi_.x0;
        digiRepeatDx_ = digiRepeatDy_ = 0;
        if (output_ != DisplayOutput::Plotter && views_isAvailable(DisplayOutput::Plotter)) {
            switchView(DisplayOutput::Plotter, plotter_, /*splash=*/false);   // (draws it, panel too)
        } else if (output_ == DisplayOutput::Plotter) {
            views_redraw();
        }
        // The marker starts at the pen -- if that's on the plot shown, else
        // in the middle (after IN the pen is at 0,0, often far off a Fit view)
        const int px = mapX(plotter_->penX()), py = mapY(plotter_->penY());
        if (px >= 0 && px < plotAreaW_ && py >= 0 && py < SCREEN_MAX_Y) digiMoveTo(px, py);
        else digiMoveTo(plotAreaW_ / 2, SCREEN_MAX_Y / 2);
        LOGF("\r\n * Plotter: digitizing -- touch the point, arrows to adjust, ENTER");
    } else if (!waiting && digiActive_) {
        digiActive_ = false;
        plotAreaW_ = SCREEN_MAX_X;
        if (output_ == DisplayOutput::Plotter) views_redraw();
        return;
    }
    if (digiActive_ && output_ == DisplayOutput::Plotter && (digiRepeatDx_ || digiRepeatDy_) &&
        time_reached(digiRepeatNext_)) {
        digiRepeatNext_ = make_timeout_time_ms(60);      // (held: about 16 steps a second)
        digiMoveTo(digiMX_ + digiRepeatDx_, digiMY_ + digiRepeatDy_);
    }
}

static void digiArrow(int dx, int dy) {
    digiMoveTo(digiMX_ + dx, digiMY_ + dy);
    digiRepeatDx_ = dx;
    digiRepeatDy_ = dy;
    digiRepeatNext_ = make_timeout_time_ms(400);
}

static void digiEnter() {
    double px, py;
    digiPoint(px, py);
    plotter_->digitize(static_cast<std::int16_t>(std::lround(px)), static_cast<std::int16_t>(std::lround(py)));
    digiRepeatDx_ = digiRepeatDy_ = 0;
    pollDigitize();                          // (the panel goes now)
    char b[40];
    std::snprintf(b, sizeof(b), "POINT %d,%d", plotter_->penX(), plotter_->penY());
    showSplashBox(b, 1, 1200);
}

bool views_digitizing() { return digiActive_ && output_ == DisplayOutput::Plotter; }

bool views_digitizeTap(int x, int y) {
    if (!views_digitizing()) return false;
    const DigiLayout& L = digi_;
    if (x < L.x0) { digiMoveTo(x, y); return true; }        // on the plot: the marker there
    if (L.up.hit(x, y))         digiArrow(0, -1);
    else if (L.down.hit(x, y))  digiArrow(0, 1);
    else if (L.left.hit(x, y))  digiArrow(-1, 0);
    else if (L.right.hit(x, y)) digiArrow(1, 0);
    else if (L.enter.hit(x, y)) digiEnter();
    else if (L.cancel.hit(x, y)) {
        plotter_->cancelDigitize();
        LOGF("\r\n * Plotter: digitizing cancelled");
        pollDigitize();
    }
    return true;                             // (the rest of the panel: nothing, but taken)
}

void views_digitizeRelease() { digiRepeatDx_ = digiRepeatDy_ = 0; }

bool views_digitizeButton(int code, bool repeat) {
    if (!views_digitizing()) return false;
    switch (code) {
        case 2: if (!repeat) digiEnter(); return true;                               // OK
        case 3: digiMoveTo(digiMX_, digiMY_ - 1); return true;                       // Up
        case 4: digiMoveTo(digiMX_, digiMY_ + 1); return true;                       // Down
        case 5: if (!repeat) { plotter_->cancelDigitize(); pollDigitize(); } return true;   // X
        default: return false;
    }
}

bool views_digitizeControl(int id, int& x, int& y) {
    digiLayout();
    const DigiRect* r = nullptr;
    switch (id) {
        case 0: r = &digi_.up; break;    case 1: r = &digi_.down; break;
        case 2: r = &digi_.left; break;  case 3: r = &digi_.right; break;
        case 4: r = &digi_.enter; break; case 5: r = &digi_.cancel; break;
        default: return false;
    }
    x = r->x + r->w / 2;
    y = r->y + r->h / 2;
    return true;
}

void views_digitizeMarker(int& x, int& y) { x = digiMX_; y = digiMY_; }

void views_plotToScreen(int px, int py, int& x, int& y) {
    x = mapX(static_cast<std::int16_t>(px));
    y = mapY(static_cast<std::int16_t>(py));
}

void views_impl::plotPoll() {
    pollDigitize();
    // The ERROR light: an error came or went
    if (output_ == DisplayOutput::Plotter && plotter_ != nullptr && plotter_->errorSerial() != errSerial_) {
        errSerial_ = plotter_->errorSerial();
        if (plotter_->errorCode() != 0) {
            plotWindow();
            drawErrorLight();
        } else if (errLit_) {
            errLit_ = false;
            views_redrawRegion(static_cast<std::int16_t>(errX()), kErrY, kErrW, kErrH);   // (the plot under it)
        }
        screen_->refreshCursor();
    }
}

void views_setPlotFit(bool fit) {
    plotFit_ = fit;
    if (output_ == DisplayOutput::Plotter) views_redraw();
}
bool views_plotFit() { return plotFit_; }

void views_setPenWide(bool wide) {
    if (plotter_ != nullptr) plotter_->setWideTip(wide);   // (what's drawn from now on)
}
bool views_penWide() { return plotter_ != nullptr && plotter_->wideTip(); }

void views_setPaperBlack(bool black) {
    paperBlack_ = black;
    if (output_ == DisplayOutput::Plotter) views_redraw();
}
bool views_paperBlack() { return paperBlack_; }

void views_clearPlotter() {
    plotter_->clear();   // resets state + segments_; fires onPlotterClear() above
}

}  // namespace hipi
