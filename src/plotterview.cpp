#include "plotterview.h"
#include "boardui.h"
#include "usb_serial.h"
#include <cmath>

namespace hp82163 {

namespace {

RA8875* display_ = nullptr;
Screen* screen_ = nullptr;
CPlotter* plotter_ = nullptr;
DisplayOutput output_ = DisplayOutput::Display;

// Fixed mapping from plotter units to screen pixels -- fits the whole
// P1..P2 hard-clip area (see plotter.cpp's OP response: 250,279 /
// 10250,7479) into the full 800x480 panel, preserving aspect ratio
// (letterboxed) since we don't yet support IP/SC user-scaling that would
// move P1/P2 at runtime. If that's ever added, this mapping needs to
// become dynamic (read the plotter's actual P1/P2 instead of these
// constants) rather than fixed like this.
constexpr double kP1X = 250.0, kP1Y = 279.0, kP2X = 10250.0, kP2Y = 7479.0;
constexpr double kPlotW = kP2X - kP1X;   // 10000
constexpr double kPlotH = kP2Y - kP1Y;   // 7200
constexpr double kScaleX = static_cast<double>(SCREEN_MAX_X) / kPlotW;
constexpr double kScaleY = static_cast<double>(SCREEN_MAX_Y) / kPlotH;
constexpr double kScale = (kScaleX < kScaleY) ? kScaleX : kScaleY;
constexpr double kFitW = kPlotW * kScale;
constexpr double kFitH = kPlotH * kScale;
constexpr double kOffsetX = (SCREEN_MAX_X - kFitW) / 2.0;
constexpr double kOffsetY = (SCREEN_MAX_Y - kFitH) / 2.0;

std::int16_t mapX(std::int16_t px) {
    // round(), not a truncating cast -- at our scale (the whole 10000x7200
    // unit P1-P2 page squeezed into 800x480 pixels, ~15 units/pixel), thin
    // details like a minus sign or a comma's tail are only 1-2 pixels to
    // begin with, so truncation's systematic downward bias makes it easy
    // to lose them entirely.
    return static_cast<std::int16_t>(std::lround(kOffsetX + (static_cast<double>(px) - kP1X) * kScale));
}

std::int16_t mapY(std::int16_t py) {
    // Flip vertical: plotter Y grows "up" (away from the origin), pixel Y
    // grows down the screen. Same rounding note as mapX() above.
    return static_cast<std::int16_t>(std::lround(kOffsetY + kFitH - (static_cast<double>(py) - kP1Y) * kScale));
}

// A handful of distinguishable pen colors (RGB565), cycling if the pen
// index exceeds the list. v1 doesn't track real per-pen colors from a
// palette-definition command (not part of our supported HP-GL set yet),
// so this is a reasonable stand-in rather than a faithful pen-color match.
constexpr std::uint16_t kPenColors[] = {
    0xFFFF, 0x07E0, 0xF800, 0x001F, 0xFFE0, 0x07FF, 0xF81F,
};
constexpr int kPenColorCount = static_cast<int>(sizeof(kPenColors) / sizeof(kPenColors[0]));

std::uint16_t penColor(std::uint8_t pen) {
    if (pen == 0) return 0x0000;  // "no pen" -- shouldn't normally draw anyway
    return kPenColors[(pen - 1) % kPenColorCount];
}

// Draws one plotter-space segment, mapped to screen pixels. If the
// mapping rounds both endpoints to the *same* screen pixel (very common
// at our scale -- roughly 15 plotter units per pixel -- for the tightly
// curved parts of glyphs like the round strokes in "6", "8", "2"), a
// straight line() call can draw nothing at all: many line-drawing engines
// (including the RA8875's own hardware one) treat identical start/end
// points as "zero pixels to step through" rather than drawing a single
// dot. Since the *plotter-space* segment is real and non-degenerate, it
// must still show up as something -- so fall back to a single pixel
// instead of silently losing it.
void drawMappedSegment(std::int16_t x0, std::int16_t y0,
                       std::int16_t x1, std::int16_t y1, std::uint16_t color) {
    const std::int16_t sx0 = mapX(x0), sy0 = mapY(y0);
    const std::int16_t sy1 = mapY(y1);
    const std::int16_t sx1 = mapX(x1);
    if (sx0 == sx1 && sy0 == sy1) {
        display_->pixel(sx0, sy0, color);
    } else {
        display_->line(sx0, sy0, sx1, sy1, color);
    }
}

// Live per-segment draw, called from CPlotter's onDraw_ callback as HP-GL
// commands stream in. No-ops entirely (segments() still records it for a
// later redraw) unless Plotter output is what's actually showing right now.
void onPlotterDraw(std::int16_t x0, std::int16_t y0,
                   std::int16_t x1, std::int16_t y1, std::uint8_t pen) {
    if (output_ != DisplayOutput::Plotter) return;
    drawMappedSegment(x0, y0, x1, y1, penColor(pen));
    // line()/pixel() go through gfxMode(), which blindly zeros MWCR0 (the
    // same text-mode/cursor-visible register Screen owns) -- same fix
    // pattern used throughout boardui.cpp for its own
    // display_->drawBitmap565()/fillRect() calls.
    screen_->refreshCursor();
}

// Fired when the plotter is reset (IN command, or a real HP-IL Clear
// Device). Only touches the screen if Plotter output is actually showing.
void onPlotterClear() {
    if (output_ != DisplayOutput::Plotter) return;
    display_->fillRect(0, 0, SCREEN_MAX_X, SCREEN_MAX_Y, 0x0000);
    screen_->refreshCursor();
}

}  // namespace

void plotterview_init(RA8875* display, Screen* screen, CPlotter* plotter) {
    display_ = display;
    screen_ = screen;
    plotter_ = plotter;
    plotter_->setDrawCallback(onPlotterDraw);
    plotter_->setClearCallback(onPlotterClear);
}

void plotterview_redraw() {
    display_->fillRect(0, 0, SCREEN_MAX_X, SCREEN_MAX_Y, 0x0000);
    for (const PlotSegment& seg : plotter_->segments()) {
        drawMappedSegment(seg.x0, seg.y0, seg.x1, seg.y1, penColor(seg.pen));
    }
    screen_->refreshCursor();
}

void plotterview_redrawRegion(std::int16_t x0, std::int16_t y0,
                              std::int16_t w, std::int16_t h) {
    // Narrow the active window to just this region -- replaying every
    // segment below then only actually draws the parts that fall inside
    // it (the RA8875 clips the rest away in hardware), avoiding the need
    // to do our own segment/rectangle intersection math.
    display_->setActiveWindow(static_cast<std::uint16_t>(x0), static_cast<std::uint16_t>(y0),
                              static_cast<std::uint16_t>(x0 + w - 1),
                              static_cast<std::uint16_t>(y0 + h - 1));
    display_->fillRect(x0, y0, w, h, 0x0000);
    for (const PlotSegment& seg : plotter_->segments()) {
        drawMappedSegment(seg.x0, seg.y0, seg.x1, seg.y1, penColor(seg.pen));
    }
    // Restore the full-screen active window Plotter mode normally runs
    // with -- the caller (boardui.cpp's hideButtonStrip()) already leaves
    // it wide at this point, so this just keeps it that way.
    display_->setActiveWindow(0, 0, SCREEN_MAX_X - 1, SCREEN_MAX_Y - 1);
    screen_->refreshCursor();
}

void plotterview_setOutput(DisplayOutput mode) {
    if (mode == output_) return;
    output_ = mode;
    if (mode == DisplayOutput::Plotter) {
        // Just stop Screen from drawing text over the plot -- suspend()
        // has no visible effect of its own. The actual redraw happens
        // exactly once, when the menu closes (see UiDialog::close()),
        // regardless of whether we just switched into Plotter mode here
        // or were already showing it. Doing a redraw here *too* just
        // drew the same thing twice in a row.
        screen_->suspend();
    }
    // Switching to Display: nothing to do here either -- UiDialog::close()
    // calls screen_->resume() itself, which un-suspends and redraws in a
    // single step.
}

DisplayOutput plotterview_output() { return output_; }

void plotterview_clearPlotter() {
    plotter_->clear();   // resets state + segments_; fires onPlotterClear() above
}

bool plotterview_isActive() { return output_ == DisplayOutput::Plotter; }

}  // namespace hp82163
