#include "plotterview.h"
#include "boardui.h"
#include "usb_serial.h"
#include "bmp_loader.hpp"
#include "drive.h"
#include <cmath>
#include <cstring>
#include <algorithm>
#include <string>
#include "pico/time.h"

namespace hipi {

namespace {

DisplayDriver* display_ = nullptr;
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
#ifndef DISPLAY_7INCH
    // FIXED: was checking screen_->isSuspended() here -- but
    // plotterview_setOutput() itself calls screen_->suspend() the moment
    // Plotter output is selected (deliberately, so Screen's own text
    // rendering doesn't draw over the plot -- see its own comment), so
    // that flag is ALWAYS true throughout completely normal Plotter
    // operation, not just while a menu happens to be open. Checking it
    // here meant this function returned immediately on every single
    // call once Plotter mode was active -- no plot data ever actually
    // reached the screen, confirmed as exactly the "tried plotting
    // something, but it doesn't work" symptom.
    //
    // On the 5" panel (no PIP hardware -- see LT7683::beginOverlayDraw()'s
    // own comment for what that means), the menu genuinely takes over the
    // one and only framebuffer while open (Screen's suspend() here is a
    // real, literal pause), so a plot draw landing on top of it WOULD
    // corrupt both -- boardui_isMenuOpen() still guards that case here.
    // On the 7" panel, drawing calls made from HP-IL processing (i.e.
    // from here, never from inside the menu's own beginOverlayDraw()/
    // endOverlayDraw() bracket) always target canvas address 0 -- the
    // live main window -- regardless of whether the menu is open, since
    // only the menu's own drawing code ever redirects the canvas
    // elsewhere. The menu shows as a PIP overlay on a separate SDRAM
    // layer instead of actually occupying the main window, so there's
    // nothing to guard against here at all: the plot keeps drawing
    // live, right through/around the menu, exactly like Screen's own
    // text does elsewhere (see uidialog.hpp's PIP work).
    if (boardui_isMenuOpen()) return;
#endif
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
#ifndef DISPLAY_7INCH
    if (boardui_isMenuOpen()) return;  // see onPlotterDraw()'s own comment
#endif
    display_->fillRect(0, 0, SCREEN_MAX_X, SCREEN_MAX_Y, 0x0000);
    screen_->refreshCursor();
}

// ── View-switch splash ──────────────────────────────────────────────────
// Briefly announces which view is now showing, whenever it actually
// changes (menu-driven or via the swipe gesture) -- non-blocking: drawn
// once here, then dismissed later by plotterview_poll() once the
// deadline passes, so a switch never freezes the main loop (HP-IL
// processing, touch handling, ...) for the splash's whole duration.
//
// 7" panel: only the small yellow box is drawn into the menu's PIP layer
// and shown as a PIP overlay, and the new view is drawn right away on the
// main window around/underneath it -- so the new content is visible at
// once, with the box on top. When the timer expires, only the overlay is
// switched off. Touch input is
// ignored while the splash is up (boardui_handleTap()), so nothing else
// can claim the PIP overlay in the meantime.
// 5" panel (no PIP): the splash is drawn directly on the panel, and the
// new view is drawn only once the timer expires, as before.
bool splashVisible_ = false;
absolute_time_t splashHideDeadline_;
constexpr std::uint32_t kSplashMs = 1500;

const char* outputName(DisplayOutput mode) {
    switch (mode) {
        case DisplayOutput::Display: return "DISPLAY";
        case DisplayOutput::Plotter: return "PLOTTER";
        case DisplayOutput::Tape:    return "TAPE";
    }
    return "?";
}

void showSwitchSplash(DisplayOutput mode) {
    display_->setActiveWindow(0, 0, SCREEN_MAX_X - 1, SCREEN_MAX_Y - 1);
#ifdef DISPLAY_7INCH
    display_->beginOverlayDraw();        // draw into the PIP layer
#else
    display_->fillRect(0, 0, SCREEN_MAX_X, SCREEN_MAX_Y, 0x0000);
#endif

    constexpr int splashW = 300, splashH = 90;   // W: multiple of 4 (PIP)
    // X rounded down to a multiple of 4 -- showPipOverlay() aligns x/w to
    // 4 pixels, so the box must be aligned too, or the PIP window would
    // show a few columns of whatever else is in the menu layer.
    constexpr int splashX = ((SCREEN_MAX_X - splashW) / 2) & ~3;
    constexpr int splashY = (SCREEN_MAX_Y - splashH) / 2;
    // Same yellow used for the menu box (see uidialog.hpp's MenuFrame) --
    // not reused directly to avoid a circular include (uidialog.hpp
    // already includes plotterview.h for DisplayOutput).
    display_->fillRect(splashX, splashY, splashW, splashH, 0xEDC0);
    display_->rect(splashX, splashY, splashW, splashH, 0xFFFF);

    const char* name = outputName(mode);
    display_->txtColor(0x0000, 0xEDC0);  // black text on the yellow box
    display_->txtSize(2);
    // Always use the chip's built-in CGROM font here, never the custom
    // hp82163_font.hpp glyphs -- same rule as every menu dialog ...
    display_->selectBuiltinFont();
    // RA8875 hardware font: 8px base glyph width, scaling to 8*(size+1)
    // per the project's own established convention (see MenuFrame's
    // "16px/char at scale 1" comment, i.e. 8*(1+1)).
    const int charW = 8 * (2 + 1);
    const int charH = 16 * (2 + 1);
    const int textW = static_cast<int>(std::strlen(name)) * charW;
    display_->txtSetCursor(splashX + (splashW - textW) / 2, splashY + (splashH - charH) / 2);
    display_->txtWrite(name);

#ifdef DISPLAY_7INCH
    display_->endOverlayDraw();          // canvas back to the live panel
    screen_->reassertRenderState();      // undo the splash's text size/colour
    display_->showPipOverlay(splashX, splashY, splashW, splashH);
#endif
    splashVisible_ = true;
    splashHideDeadline_ = make_timeout_time_ms(kSplashMs);
}

// ─── Tape view (7" panel only) ──────────────────────────────────────────
// hp82161a.bmp is drawn ONCE at boot into its own off-screen SDRAM layer
// (LT7683::kTapeLayerAddr), centered and surrounded by black. Showing the
// view, or restoring a part of it (e.g. after the button strip hides), is
// then a single in-chip BTE copy -- no SD card or SPI pixel traffic.
// The RA8875 (5") has no spare layer at 16bpp, so the view isn't offered
// there at all (tapeLoaded_ stays false).
constexpr const char* kTapeBmpPath = "hp82161a.bmp";
bool tapeLoaded_ = false;
std::int16_t tapeX_ = 0, tapeY_ = 0;     // image's top-left on screen
[[maybe_unused]] std::uint16_t tapeW_ = 0, tapeH_ = 0;    // image size (unused on 5")

// Touch-sensitive areas, in hp82161a.bmp's OWN pixel coordinates (0,0 = the
// image's top-left corner), so they stay correct wherever the image is
// centered. Measured on the 1024x600 HP82161A picture (drive on the left,
// light background on the right where the button strip slides in) --
// if hp82161a.bmp is replaced by a different picture, these must be re-measured.
struct TapeHotspotRect {
    TapeHotspot id;
    std::int16_t x0, y0, x1, y1;         // inclusive-exclusive
};
constexpr TapeHotspotRect kTapeHotspots[] = {
    { TapeHotspot::Open, 535, 145, 757, 278 },   // cassette window
    { TapeHotspot::Open, 668, 448, 745, 525 },   // OPEN button + its label
    { TapeHotspot::Power, 112, 452, 208, 500 },  // OFF-STANDBY-ON switch
};

#ifdef DISPLAY_7INCH
void loadTapeLayer() {
    std::uint16_t w = 0, h = 0;
    if (!peekBmpDimensions(kTapeBmpPath, w, h)) {
        LOGF("\r\n\t* %s not found -- Tape view disabled", kTapeBmpPath);
        return;
    }
    if (w > SCREEN_MAX_X || h > SCREEN_MAX_Y) {
        LOGF("\r\n\t* %s is %ux%u, larger than the panel -- Tape view disabled",
             kTapeBmpPath, w, h);
        return;
    }
    tapeW_ = w;
    tapeH_ = h;
    tapeX_ = static_cast<std::int16_t>((SCREEN_MAX_X - w) / 2);
    tapeY_ = static_cast<std::int16_t>((SCREEN_MAX_Y - h) / 2);

    display_->setActiveWindow(0, 0, SCREEN_MAX_X - 1, SCREEN_MAX_Y - 1);
    display_->beginLayerDraw(LT7683::kTapeLayerAddr);
    display_->fillRect(0, 0, SCREEN_MAX_X, SCREEN_MAX_Y, 0x0000);
    tapeLoaded_ = drawBmpAt(display_, kTapeBmpPath, tapeX_, tapeY_);
    display_->endOverlayDraw();          // canvas back to the live panel
    screen_->reassertRenderState();      // same as UiDialog after overlay drawing
    LOGF("\r\n\t* Tape view %s", tapeLoaded_ ? "ready" : "disabled (bad BMP)");
}
#endif

// ─── Cassette in the Tape view (7" only) ────────────────────────────────
// tape-in.bmp is a picture of an HP cassette, pre-cut to exactly the
// window opening in the drive's lid (see hp82161a.bmp), with its reel hubs
// on the drive's two pins. It is loaded once at boot into
// LT7683::kCassetteAddr. Whenever the selected file changes, it is copied
// to kCassetteLabelAddr and the file name is written on the label; that
// copy is what's shown. The cassette is shown when a file is selected and
// the file picker isn't open ("ejected" while choosing a file).
constexpr const char* kCassetteBmpPath = "tape-in.bmp";
// Window opening in hp82161a.bmp's own pixel coordinates (= tape-in.bmp's
// top-left). Re-measure if hp82161a.bmp is replaced.
constexpr std::int16_t kCassetteX = 545, kCassetteY = 158;
// File name (without extension) on the label, in tape-in.bmp coordinates,
// built-in 8x16 font, fake-bold (drawn twice, 1 px apart). Written like on
// a real label: starting on the upper line -- right of the hp logo, above
// the drive pins -- and continuing on the lower field under the reels if
// it doesn't fit. Both lines are left-aligned.
constexpr std::int16_t kLabelTopX   = 43, kLabelTopY   = 8;       // upper line
constexpr std::size_t  kLabelTopMax = 16;
constexpr std::int16_t kLabelLowX   = 12, kLabelLowY   = 86;      // lower field
constexpr std::size_t  kLabelLowMax = 21;
constexpr std::uint16_t kLabelInk = 0x2104;   // near-black "pen"

// Open lid: open.bmp replaces the lid area (lid raised, empty drive
// with its pins) while the drive is "ejected", i.e. from pressing OPEN (or
// opening the file picker any other way) until the picker closes.
// Coordinates in hp82161a.bmp's own pixels -- re-measure if hp82161a.bmp changes.
constexpr const char* kTapeOpenBmpPath = "open.bmp";
constexpr std::int16_t kTapeOpenX = 496, kTapeOpenY = 0;
bool lidOpenLoaded_ = false;
std::uint16_t lidOpenW_ = 0, lidOpenH_ = 0;
[[maybe_unused]] std::uint16_t lidOpenStride_ = 0;   // (unused on 5")

// Status sprites: leds.bmp holds, side by side, the POWER and BUSY LEDs
// lit (each a 40x40 cut-out of hp82161a.bmp with a red glow added) and
// the power switch in its ON position (80x32). Shown = copy the sprite;
// not shown = restore that area from hp82161a.bmp's layer (LEDs off,
// switch OFF, as photographed).
constexpr const char* kLedsBmpPath = "leds.bmp";
constexpr std::int16_t kLedSize = 40;
struct LedSprite { std::int16_t x, y, w, h, srcX; };   // screen rect, column in leds.bmp
constexpr LedSprite kPowerLed {257, 455, 40, 40,  0};
constexpr LedSprite kBusyLed  {454, 455, 40, 40, 40};
constexpr LedSprite kSwitchOn {120, 460, 80, 32, 80};
[[maybe_unused]] bool switchSpriteLoaded_ = false;  // leds.bmp wide enough for the switch (unused on 5")
bool ledsLoaded_ = false;
[[maybe_unused]] std::uint16_t ledsStride_ = 0;  // (unused on 5")
bool powerLit_ = false, busyLit_ = false;
CDrive* drive_ = nullptr;

bool cassetteLoaded_ = false;
std::uint16_t cassetteW_ = 0, cassetteH_ = 0;
std::string tapeFile_;          // selected .dat file, "" = none
bool tapeEjected_ = false;      // true while the file picker is open

[[maybe_unused]] bool cassetteShown() {   // (unused on 5")
    return cassetteLoaded_ && !tapeFile_.empty() && !tapeEjected_;
}

#ifdef DISPLAY_7INCH
// Draws into another layer and afterwards restores whatever canvas was
// active before -- e.g. the menu's layer, when a file is chosen from the
// menu while it is being drawn.
class LayerDrawScope {
public:
    explicit LayerDrawScope(std::uint32_t addr, std::uint16_t stride = 0)
        : prevAddr_(display_->canvasAddr()), prevStride_(display_->canvasStride()) {
        display_->beginLayerDraw(addr, stride);
    }
    ~LayerDrawScope() {
        if (prevAddr_ == 0) display_->endOverlayDraw();
        else                display_->beginLayerDraw(prevAddr_, prevStride_);
    }
    LayerDrawScope(const LayerDrawScope&) = delete;
    LayerDrawScope& operator=(const LayerDrawScope&) = delete;
private:
    std::uint32_t prevAddr_;
    std::uint16_t prevStride_;
};

void loadCassette() {
    std::uint16_t w = 0, h = 0;
    if (!peekBmpDimensions(kCassetteBmpPath, w, h)) {
        LOGF("\r\n\t* %s not found -- no cassette in the Tape view", kCassetteBmpPath);
        return;
    }
    if (h > LT7683::kCassetteMaxRows || kCassetteX + w > SCREEN_MAX_X) {
        LOGF("\r\n\t* %s is %ux%u, too large -- no cassette in the Tape view",
             kCassetteBmpPath, w, h);
        return;
    }
    {
        LayerDrawScope scope(LT7683::kCassetteAddr);
        cassetteLoaded_ = drawBmpAt(display_, kCassetteBmpPath, 0, 0);
    }
    screen_->reassertRenderState();
    cassetteW_ = w;
    cassetteH_ = h;
}

void loadLidOpen() {
    std::uint16_t w = 0, h = 0;
    if (!peekBmpDimensions(kTapeOpenBmpPath, w, h)) {
        LOGF("\r\n\t* %s not found -- lid stays closed in the Tape view", kTapeOpenBmpPath);
        return;
    }
    const std::uint16_t stride = static_cast<std::uint16_t>((w + 3) & ~3);   // PIP/BTE: 4-px units
    if (kTapeOpenX + w > SCREEN_MAX_X || kTapeOpenY + h > SCREEN_MAX_Y ||
        static_cast<std::uint32_t>(stride) * h * 2 > LT7683::kTapeOpenMaxBytes) {
        LOGF("\r\n\t* %s is %ux%u, too large -- lid stays closed", kTapeOpenBmpPath, w, h);
        return;
    }
    {
        // Narrow layer: the active window must match its own width while
        // drawing (same as the button strip's layer in boardui.cpp)
        LayerDrawScope scope(LT7683::kTapeOpenAddr, stride);
        display_->setActiveWindow(0, 0, stride - 1, SCREEN_MAX_Y - 1);
        lidOpenLoaded_ = drawBmpAt(display_, kTapeOpenBmpPath, 0, 0);
    }
    display_->setActiveWindow(0, 0, SCREEN_MAX_X - 1, SCREEN_MAX_Y - 1);
    screen_->reassertRenderState();
    lidOpenW_ = w;
    lidOpenH_ = h;
    lidOpenStride_ = stride;
}

// Copies the part of a pre-rendered patch (layer srcAddr, own stride,
// drawn at screen position px/py, size pw x ph) that falls inside the
// region x/y/w/h onto the panel.
void overlayPatch(std::uint32_t srcAddr, std::uint16_t srcStride,
                  int px, int py, int pw, int ph,
                  int x, int y, int w, int h, int srcX0 = 0) {
    const int ix0 = std::max(x, px), iy0 = std::max(y, py);
    const int ix1 = std::min(x + w, px + pw), iy1 = std::min(y + h, py + ph);
    if (ix1 <= ix0 || iy1 <= iy0) return;
    display_->copyLayerRegion(srcAddr, srcStride,
                              static_cast<std::int16_t>(srcX0 + ix0 - px),
                              static_cast<std::int16_t>(iy0 - py),
                              /*dstAddr=*/0, SCREEN_MAX_X,
                              static_cast<std::int16_t>(ix0),
                              static_cast<std::int16_t>(iy0),
                              static_cast<std::uint16_t>(ix1 - ix0),
                              static_cast<std::uint16_t>(iy1 - iy0));
}

void loadLeds() {
    std::uint16_t w = 0, h = 0;
    if (!peekBmpDimensions(kLedsBmpPath, w, h)) {
        LOGF("\r\n\t* %s not found -- no status LEDs in the Tape view", kLedsBmpPath);
        return;
    }
    if (w < 2 * kLedSize || h < kLedSize || w > 256 || h > 64) {
        LOGF("\r\n\t* %s is %ux%u, expected %ux%u -- no status LEDs",
             kLedsBmpPath, w, h, 2 * kLedSize, kLedSize);
        return;
    }
    const std::uint16_t stride = static_cast<std::uint16_t>((w + 3) & ~3);
    {
        LayerDrawScope scope(LT7683::kLedsAddr, stride);
        display_->setActiveWindow(0, 0, stride - 1, SCREEN_MAX_Y - 1);
        ledsLoaded_ = drawBmpAt(display_, kLedsBmpPath, 0, 0);
    }
    display_->setActiveWindow(0, 0, SCREEN_MAX_X - 1, SCREEN_MAX_Y - 1);
    screen_->reassertRenderState();
    ledsStride_ = stride;
    switchSpriteLoaded_ = ledsLoaded_ && w >= kSwitchOn.srcX + kSwitchOn.w && h >= kSwitchOn.h;
}

// Rebuilds the shown cassette: fresh copy of the picture + file name
void composeCassetteLabel() {
    if (!cassetteLoaded_) return;
    display_->copyLayerRegion(LT7683::kCassetteAddr, SCREEN_MAX_X, 0, 0,
                              LT7683::kCassetteLabelAddr, SCREEN_MAX_X, 0, 0,
                              cassetteW_, cassetteH_);
    if (tapeFile_.empty()) return;

    // Drop the extension ("GAMES.DAT" -> "GAMES")
    std::string name = tapeFile_;
    const std::size_t dot = name.find_last_of('.');
    if (dot != std::string::npos && dot > 0) name.resize(dot);

    // Split: the upper line gets as much as fits, the rest continues on
    // the lower line. Break after a separator if one keeps both lines
    // within their limits, so words aren't cut; else cut at the limit.
    std::string upper = name, lower;
    if (name.size() > kLabelTopMax) {
        std::size_t split = kLabelTopMax;
        const std::size_t minSplit = name.size() > kLabelLowMax ? name.size() - kLabelLowMax : 1;
        for (std::size_t i = kLabelTopMax; i >= std::max<std::size_t>(minSplit, 1); --i) {
            const char c = name[i - 1];
            if (c == ' ' || c == '_' || c == '-') { split = i; break; }
        }
        upper = name.substr(0, split);
        lower = name.substr(split);
        while (!upper.empty() && upper.back() == ' ') upper.pop_back();   // no dangling space
        if (lower.size() > kLabelLowMax) lower.resize(kLabelLowMax);      // too long for both
    }

    {
        LayerDrawScope scope(LT7683::kCassetteLabelAddr);
        display_->selectBuiltinFont();
        display_->txtSize(0);                 // 8x16
        display_->txtTrans(kLabelInk);        // transparent background
        // Fake bold: the same text twice, the second pass 1 px to the right
        auto writeBold = [](const std::string& s, int x, int y) {
            for (int dx = 0; dx <= 1; ++dx) {
                display_->txtSetCursor(static_cast<std::uint16_t>(x + dx),
                                       static_cast<std::uint16_t>(y));
                display_->txtWrite(s.c_str());
            }
        };
        writeBold(upper, kLabelTopX, kLabelTopY);
        if (!lower.empty()) writeBold(lower, kLabelLowX, kLabelLowY);
    }
    screen_->reassertRenderState();           // Screen's own text colour/size
}
#endif

void drawTapeRegion(std::int16_t x, std::int16_t y, std::int16_t w, std::int16_t h) {
#ifdef DISPLAY_7INCH
    display_->setActiveWindow(0, 0, SCREEN_MAX_X - 1, SCREEN_MAX_Y - 1);
    display_->copyLayerToPanel(LT7683::kTapeLayerAddr, x, y,
                               static_cast<std::uint16_t>(w),
                               static_cast<std::uint16_t>(h));
    if (tapeEjected_ && lidOpenLoaded_) {
        // Lid open (drive empty) -- see kTapeOpenBmpPath
        overlayPatch(LT7683::kTapeOpenAddr, lidOpenStride_,
                     tapeX_ + kTapeOpenX, tapeY_ + kTapeOpenY, lidOpenW_, lidOpenH_,
                     x, y, w, h);
    } else if (cassetteShown()) {
        // Cassette with the file name, seen through the lid window
        overlayPatch(LT7683::kCassetteLabelAddr, SCREEN_MAX_X,
                     tapeX_ + kCassetteX, tapeY_ + kCassetteY, cassetteW_, cassetteH_,
                     x, y, w, h);
    }
    if (ledsLoaded_) {
        for (const auto& [led, lit] : {std::pair{kPowerLed, powerLit_},
                                       std::pair{kBusyLed,  busyLit_},
                                       std::pair{kSwitchOn, powerLit_ && switchSpriteLoaded_}}) {
            if (!lit) continue;                  // off = the base image already copied
            overlayPatch(LT7683::kLedsAddr, ledsStride_,
                         tapeX_ + led.x, tapeY_ + led.y, led.w, led.h,
                         x, y, w, h, led.srcX);
        }
    }
    screen_->refreshCursor();   // BTE switched the chip to graphics mode --
                                // same MWCR0 restore as the plotter paths
#else
    (void)x; (void)y; (void)w; (void)h;
#endif
}

// Redraws one status LED square if the Tape view is showing
void refreshLed(const LedSprite& led) {
    if (output_ != DisplayOutput::Tape) return;
    drawTapeRegion(static_cast<std::int16_t>(tapeX_ + led.x),
                   static_cast<std::int16_t>(tapeY_ + led.y), led.w, led.h);
}

// Redraws the lid area (open lid, cassette or empty window) if the Tape
// view is showing. The open-lid patch covers the cassette window too.
void refreshCassette() {
    if (output_ != DisplayOutput::Tape) return;
    if (lidOpenLoaded_) {
        drawTapeRegion(static_cast<std::int16_t>(tapeX_ + kTapeOpenX),
                       static_cast<std::int16_t>(tapeY_ + kTapeOpenY),
                       static_cast<std::int16_t>(lidOpenW_),
                       static_cast<std::int16_t>(lidOpenH_));
    } else if (cassetteLoaded_) {
        drawTapeRegion(static_cast<std::int16_t>(tapeX_ + kCassetteX),
                       static_cast<std::int16_t>(tapeY_ + kCassetteY),
                       static_cast<std::int16_t>(cassetteW_),
                       static_cast<std::int16_t>(cassetteH_));
    }
}

// Draws whichever view is now current on the main window: resumes Screen
// for Display (its own full() redraw), otherwise the plot or Tape image.
void revealCurrentView() {
    if (output_ == DisplayOutput::Display) {
        screen_->resume();       // un-suspends + its own full() redraw
    } else {
        plotterview_redraw();    // plot or Tape image
    }
}

}  // namespace

void plotterview_init(DisplayDriver* display, Screen* screen, CPlotter* plotter) {
    display_ = display;
    screen_ = screen;
    plotter_ = plotter;
    plotter_->setDrawCallback(onPlotterDraw);
    plotter_->setClearCallback(onPlotterClear);
#ifdef DISPLAY_7INCH
    loadTapeLayer();
    if (tapeLoaded_) {
        loadCassette();
        loadLidOpen();
        loadLeds();
    }
#endif
}

void plotterview_setTapeFile(const std::string& filename) {
    tapeFile_ = filename;
#ifdef DISPLAY_7INCH
    composeCassetteLabel();
#endif
    refreshCassette();
}

void plotterview_setDrive(CDrive* drive) {
    drive_ = drive;
}

CDrive* plotterview_drive() {
    return drive_;
}

void plotterview_setTapeEjected(bool ejected) {
    if (ejected == tapeEjected_) return;
    tapeEjected_ = ejected;
    refreshCassette();
}

bool plotterview_isAvailable(DisplayOutput mode) {
    return mode != DisplayOutput::Tape || tapeLoaded_;
}

void plotterview_redraw() {
    if (output_ == DisplayOutput::Tape) {
        drawTapeRegion(0, 0, SCREEN_MAX_X, SCREEN_MAX_Y);
        return;
    }
    if (output_ != DisplayOutput::Plotter) return;
    display_->fillRect(0, 0, SCREEN_MAX_X, SCREEN_MAX_Y, 0x0000);
    for (const PlotSegment& seg : plotter_->segments()) {
        drawMappedSegment(seg.x0, seg.y0, seg.x1, seg.y1, penColor(seg.pen));
    }
    screen_->refreshCursor();
}

void plotterview_redrawRegion(std::int16_t x0, std::int16_t y0,
                              std::int16_t w, std::int16_t h) {
    if (output_ == DisplayOutput::Tape) {
        drawTapeRegion(x0, y0, w, h);
        return;
    }
    if (output_ != DisplayOutput::Plotter) return;
    // Narrow the active window to just this region -- replaying every
    // segment below then only actually draws the parts that fall inside
    // it (the LT7683/RA8875 clip the rest away in hardware), avoiding the
    // need to do our own segment/rectangle intersection math for the
    // PIXELS actually drawn.
    display_->setActiveWindow(static_cast<std::uint16_t>(x0), static_cast<std::uint16_t>(y0),
                              static_cast<std::uint16_t>(x0 + w - 1),
                              static_cast<std::uint16_t>(y0 + h - 1));
    display_->fillRect(x0, y0, w, h, 0x0000);
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
        drawMappedSegment(seg.x0, seg.y0, seg.x1, seg.y1, penColor(seg.pen));
    }
    // Restore the full-screen active window Plotter mode normally runs
    // with -- the caller (boardui.cpp's hideButtonStrip()) already leaves
    // it wide at this point, so this just keeps it that way.
    display_->setActiveWindow(0, 0, SCREEN_MAX_X - 1, SCREEN_MAX_Y - 1);
    screen_->refreshCursor();
}

void plotterview_setOutput(DisplayOutput mode) {
    if (mode == output_ || !plotterview_isAvailable(mode)) return;
    output_ = mode;
    if (mode != DisplayOutput::Display) {
        // Stop Screen from drawing text over the plot, and turn off the
        // hardware cursor explicitly -- suspend() alone doesn't touch
        // whatever cursor state the RA8875 already had active, which
        // would otherwise just keep blinking autonomously via the chip's
        // own hardware timer regardless of software suspension.
        screen_->suspend();
        screen_->hideCursorHardware();
    }
    showSwitchSplash(mode);
#ifdef DISPLAY_7INCH
    // Draw the new view now, hidden behind the full-screen splash overlay
    // -- it's already complete when plotterview_poll() removes the overlay.
    revealCurrentView();
#endif
    // 5": deliberately do NOT draw the new view (or resume() Screen) here
    // -- the splash is drawn directly on the panel and would be
    // overwritten. plotterview_poll() does it once the splash times out.
}

bool plotterview_isSplashVisible() { return splashVisible_; }

void plotterview_poll() {
    // Status LEDs -- redraw only on change (a single BTE copy each)
    if (ledsLoaded_ && drive_ != nullptr) {
        const bool power = drive_->enabled();
        const bool busy  = power && drive_->isBusy();
        if (power != powerLit_) {
            powerLit_ = power;
            refreshLed(kPowerLed);
            refreshLed(kSwitchOn);           // switch follows POWER
        }
        if (busy  != busyLit_)  { busyLit_  = busy;  refreshLed(kBusyLed); }
    }
    if (splashVisible_ && time_reached(splashHideDeadline_)) {
        splashVisible_ = false;
#ifdef DISPLAY_7INCH
        display_->hidePipOverlay();      // view underneath is already drawn
#else
        revealCurrentView();
#endif
    }
}

DisplayOutput plotterview_output() { return output_; }

void plotterview_cycleOutput(bool forward) {
    // Proper modulo for the backward case too (a plain % can return a
    // negative result in C++ for a negative left-hand side).
    // Skip views that aren't available (Tape on the 5" panel, or with no
    // hp82161a.bmp) -- Display is always available, so this always terminates.
    const int step = forward ? 1 : -1;
    int next = static_cast<int>(output_);
    do {
        next = ((next + step) % kDisplayOutputCount + kDisplayOutputCount) % kDisplayOutputCount;
    } while (!plotterview_isAvailable(static_cast<DisplayOutput>(next)));
    plotterview_setOutput(static_cast<DisplayOutput>(next));
}

void plotterview_clearPlotter() {
    plotter_->clear();   // resets state + segments_; fires onPlotterClear() above
}

bool plotterview_isActive() { return output_ != DisplayOutput::Display; }

TapeHotspot plotterview_tapeHitTest(std::uint16_t x, std::uint16_t y) {
    if (output_ != DisplayOutput::Tape || !tapeLoaded_ || splashVisible_) {
        return TapeHotspot::None;
    }
    const int ix = static_cast<int>(x) - tapeX_;   // screen -> image coordinates
    const int iy = static_cast<int>(y) - tapeY_;
    for (const TapeHotspotRect& r : kTapeHotspots) {
        if (ix >= r.x0 && ix < r.x1 && iy >= r.y0 && iy < r.y1) return r.id;
    }
    return TapeHotspot::None;
}

}  // namespace hipi
