#include "plotterview.h"
#include "sd_paths.h"
#include "boardui.h"
#include "usb_serial.h"
#include "bmp_loader.hpp"
#include "display_mirror.h"   // layers a PC viewer needs (tools/hipiview)
#include "drive.h"
#include "analyzer.h"
#include "clock.h"
#include "loopmap.h"
#include "hpil_diag.h"
#include "chartable.h"
#include "backlight.h"
#include <cctype>
#include <vector>
extern std::vector<CDevice*> devices;   // hipi.cpp -- loop order
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
// The device whose view is showing (nullptr = none yet / the Display view
// with no display device) -- swiping steps through the devices from here
CDevice* viewDevice_ = nullptr;

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

// The mapping in use. Two ways to show the plot (Display -> Plot view):
//  Page -- the whole P1..P2 area (10000 x 7200 units) fitted to the screen,
//          centred: what the paper would show, but the HP-41's plots don't
//          sit in the middle of it (room for the axis labels, left/bottom).
//  Fit  -- the plot itself (the bounding box of everything drawn, plus a
//          margin) fitted to the screen and centred, so it uses the screen
//          best. Grows (with some slack) as the plot grows.
// Both keep the aspect ratio -- circles stay round, text undistorted.
double mapOX_ = kP1X, mapOY_ = kP1Y;      // plotter units at the content's left / bottom
double mapScale_ = kScale;                // pixels per plotter unit
double mapDX_ = kOffsetX, mapDY_ = kOffsetY, mapH_ = kFitH;
double fitX0_ = 0, fitY0_ = 0, fitX1_ = -1, fitY1_ = -1;   // Fit: the area shown (units)

void setPageMapping() {
    mapOX_ = kP1X; mapOY_ = kP1Y; mapScale_ = kScale;
    mapDX_ = kOffsetX; mapDY_ = kOffsetY; mapH_ = kFitH;
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
    double s = std::min(SCREEN_MAX_X / w, SCREEN_MAX_Y / h);
    s = std::min(s, kScale * 4);                       // a tiny plot: not more than 4x the page
    mapScale_ = s;
    mapOX_ = fitX0_;
    mapOY_ = fitY0_;
    mapH_ = h * s;
    mapDX_ = (SCREEN_MAX_X - w * s) / 2;
    mapDY_ = (SCREEN_MAX_Y - mapH_) / 2;
}

bool plotFit_ = true;                     // Display -> Plot view (config plot_fit)

std::int16_t mapX(std::int16_t px) {
    // round(), not a truncating cast -- at our scale (the whole 10000x7200
    // unit P1-P2 page squeezed into 800x480 pixels, ~15 units/pixel), thin
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

// The plot always covers the whole panel. On the 7" panel the button strip
// is a PIP overlay, so the panel underneath belongs to the plot too -- but
// showing the strip (or updating a LED on it) leaves the active window
// narrowed to the area left of it, which would clip the plot there: fills
// and lines under the strip were left out, and showed once the strip went
// away (e.g. still the old paper colour after a Paper change). So widen it
// before drawing. (5": the strip really covers that area; hiding it
// redraws the region -- plotterview_redrawRegion().)
void plotWindow() {
#ifdef DISPLAY_7INCH
    display_->setActiveWindow(0, 0, SCREEN_MAX_X - 1, SCREEN_MAX_Y - 1);
#endif
}

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
#ifndef DISPLAY_7INCH
        if (!boardui_isMenuOpen())                // (5": the menu owns the screen while open)
#endif
        {
            plotterview_redraw();                 // (refits to all segments, this one included)
            return;
        }
    }
#ifndef DISPLAY_7INCH
    // FIXED: was checking screen_->isSuspended() here -- but
    // switchView() itself calls screen_->suspend() the moment
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
    plotWindow();
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
    plotWindow();
    display_->fillRect(0, 0, SCREEN_MAX_X, SCREEN_MAX_Y, paperColor());
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
        case DisplayOutput::Analyzer: return "ANALYZER";
        case DisplayOutput::Clock:    return "CLOCK";
        case DisplayOutput::LoopMap:  return "LOOP MAP";
        case DisplayOutput::Signals:  return "HP-IL SIGNALS";
        case DisplayOutput::CharTable: return "CHARACTERS";
    }
    return "?";
}

// The yellow box used for the view-switch splash and for short messages
// (plotterview_showMessage()): `text` centred at the given built-in font
// scale, shown for `ms` -- plotterview_poll() removes it.
void showSplashBox(const char* text, std::uint8_t scale, std::uint32_t ms) {
    display_->setActiveWindow(0, 0, SCREEN_MAX_X - 1, SCREEN_MAX_Y - 1);
#ifdef DISPLAY_7INCH
    display_->beginOverlayDraw();        // draw into the PIP layer
#else
    display_->fillRect(0, 0, SCREEN_MAX_X, SCREEN_MAX_Y, 0x0000);
#endif

    // RA8875/LT7683 hardware font: 8px base glyph width, scaling to
    // 8*(size+1) per the project's own established convention (see
    // MenuFrame's "16px/char at scale 1" comment, i.e. 8*(1+1)).
    const int charW = 8 * (scale + 1);
    const int charH = 16 * (scale + 1);
    const int textW = static_cast<int>(std::strlen(text)) * charW;
    // Box: at least 300 wide, wider for long messages; W a multiple of 4
    const int splashW = std::min((std::max(300, textW + 48) + 3) & ~3, (SCREEN_MAX_X - 32) & ~3);
    const int splashH = charH + 42;
    // X rounded down to a multiple of 4 -- showPipOverlay() aligns x/w to
    // 4 pixels, so the box must be aligned too, or the PIP window would
    // show a few columns of whatever else is in the menu layer.
    const int splashX = ((SCREEN_MAX_X - splashW) / 2) & ~3;
    const int splashY = (SCREEN_MAX_Y - splashH) / 2;
    // Same yellow used for the menu box (see uidialog.hpp's MenuFrame) --
    // not reused directly to avoid a circular include (uidialog.hpp
    // already includes plotterview.h for DisplayOutput).
    display_->fillRect(splashX, splashY, splashW, splashH, 0xEDC0);
    display_->rect(splashX, splashY, splashW, splashH, 0xFFFF);

    display_->txtColor(0x0000, 0xEDC0);  // black text on the yellow box
    display_->txtSize(scale);
    // Always use the chip's built-in CGROM font here, never the custom
    // hp82163_font.hpp glyphs -- same rule as every menu dialog ...
    display_->selectBuiltinFont();
    display_->txtSetCursor(splashX + (splashW - textW) / 2, splashY + (splashH - charH) / 2);
    display_->txtWrite(text);

#ifdef DISPLAY_7INCH
    display_->endOverlayDraw();          // canvas back to the live panel
    screen_->reassertRenderState();      // undo the splash's text size/colour
    display_->showPipOverlay(splashX, splashY, splashW, splashH);
#endif
    splashVisible_ = true;
    splashHideDeadline_ = make_timeout_time_ms(ms);
}

DisplayOutput outputFor(ViewKind k) {
    switch (k) {
        case ViewKind::Plotter: return DisplayOutput::Plotter;
        case ViewKind::Tape:    return DisplayOutput::Tape;
        default:                return DisplayOutput::Display;
    }
}

// On the loop, switched on, and its kind of view can be shown here
bool deviceHasView(CDevice* dev) {
    return dev != nullptr && dev->enabled() && dev->viewKind() != ViewKind::None &&
           plotterview_isAvailable(outputFor(dev->viewKind()));
}

// ─── Tape view (7" panel only) ──────────────────────────────────────────
// hp82161a.bmp is drawn ONCE at boot into its own off-screen SDRAM layer
// (LT7683::kTapeLayerAddr), centered and surrounded by black. Showing the
// view, or restoring a part of it (e.g. after the button strip hides), is
// then a single in-chip BTE copy -- no SD card or SPI pixel traffic.
// The RA8875 (5") has no spare layer at 16bpp, so the view isn't offered
// there at all (tapeLoaded_ stays false).
constexpr const char* kTapeBmpPath = HIPI_PATH(HIPI_DIR_RESOURCES, "hp82161a.bmp");
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
    { TapeHotspot::Rewind, 540, 448, 612, 525 }, // REWIND button + its label
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
    if (tapeLoaded_)                     // a fixed picture: cached by the viewer
        displayMirror_addLayer(LT7683::kTapeLayerAddr, SCREEN_MAX_X * 2UL * SCREEN_MAX_Y, 0, 1,
                               displayMirror_fileAssetId(kTapeBmpPath, static_cast<std::uint32_t>(tapeX_) | (static_cast<std::uint32_t>(tapeY_) << 16)));
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
constexpr const char* kCassetteBmpPath = HIPI_PATH(HIPI_DIR_RESOURCES, "tape-in.bmp");
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
constexpr const char* kTapeOpenBmpPath = HIPI_PATH(HIPI_DIR_RESOURCES, "open.bmp");
constexpr std::int16_t kTapeOpenX = 496, kTapeOpenY = 0;
bool lidOpenLoaded_ = false;
std::uint16_t lidOpenW_ = 0, lidOpenH_ = 0;
[[maybe_unused]] std::uint16_t lidOpenStride_ = 0;   // (unused on 5")

// Status sprites: leds.bmp holds, side by side, the POWER and BUSY LEDs
// lit (each a 40x40 cut-out of hp82161a.bmp with a red glow added) and
// the power switch in its ON position (80x32). Shown = copy the sprite;
// not shown = restore that area from hp82161a.bmp's layer (LEDs off,
// switch OFF, as photographed).
constexpr const char* kLedsBmpPath = HIPI_PATH(HIPI_DIR_RESOURCES, "leds.bmp");
constexpr std::int16_t kLedSize = 40;
struct LedSprite { std::int16_t x, y, w, h, srcX; };   // screen rect, column in leds.bmp
constexpr LedSprite kPowerLed {257, 455, 40, 40,  0};
constexpr LedSprite kBusyLed  {454, 455, 40, 40, 40};
constexpr LedSprite kSwitchOn      {120, 460, 80, 32,  80};
constexpr LedSprite kSwitchStandby {120, 460, 80, 32, 160};
[[maybe_unused]] bool switchSpriteLoaded_ = false;   // leds.bmp has the ON switch (unused on 5")
[[maybe_unused]] bool standbySpriteLoaded_ = false;  // ... and the STANDBY switch
DrivePower switchShown_ = DrivePower::Off;           // switch position currently drawn
bool ledsLoaded_ = false;
[[maybe_unused]] std::uint16_t ledsStride_ = 0;  // (unused on 5")
bool powerLit_ = false, busyLit_ = false;
CDrive* drive_ = nullptr;

// Spinning reels: reels.bmp holds kReelFrames frames of each reel hub
// (38x38, cut out of tape-in.bmp and rotated in 15-degree steps -- the hub
// has six holes, so four frames make a seamless 60-degree cycle), one row
// per hub. While the drive is busy (reading, writing, seeking) and the
// cassette is shown, the frames are cycled; frame 0 is the picture itself.
constexpr const char* kReelsBmpPath = HIPI_PATH(HIPI_DIR_RESOURCES, "reels.bmp");
constexpr std::int16_t kReelBox = 38;
constexpr int kReelFrames = 4;
constexpr std::uint32_t kReelFrameMs = 60;
constexpr std::uint32_t kReelRewindFrameMs = 25;   // rewind runs ~3x faster
struct ReelPos { std::int16_t x, y; };               // box position in tape-in.bmp
constexpr ReelPos kReels[2] = { {17, 38}, {143, 38} };
bool reelsLoaded_ = false;
[[maybe_unused]] std::uint16_t reelsStride_ = 0;    // (unused on 5")
int reelFrame_ = 0;
absolute_time_t reelNext_ = nil_time;

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
    if (cassetteLoaded_) {
        // The picture is fixed (cached by a PC viewer); its copy with the
        // file name on the label changes at run time
        displayMirror_addLayer(LT7683::kCassetteAddr, w * 2UL, SCREEN_MAX_X * 2UL, h,
                               displayMirror_fileAssetId(kCassetteBmpPath, 1));
        displayMirror_addLayer(LT7683::kCassetteLabelAddr, w * 2UL, SCREEN_MAX_X * 2UL, h, 0);
    }
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
    if (lidOpenLoaded_)
        displayMirror_addLayer(LT7683::kTapeOpenAddr, stride * 2UL * h, 0, 1,
                               displayMirror_fileAssetId(kTapeOpenBmpPath, stride));
}

// Copies the part of a pre-rendered patch (layer srcAddr, own stride,
// drawn at screen position px/py, size pw x ph) that falls inside the
// region x/y/w/h onto the panel.
void overlayPatch(std::uint32_t srcAddr, std::uint16_t srcStride,
                  int px, int py, int pw, int ph,
                  int x, int y, int w, int h, int srcX0 = 0, int srcY0 = 0) {
    const int ix0 = std::max(x, px), iy0 = std::max(y, py);
    const int ix1 = std::min(x + w, px + pw), iy1 = std::min(y + h, py + ph);
    if (ix1 <= ix0 || iy1 <= iy0) return;
    display_->copyLayerRegion(srcAddr, srcStride,
                              static_cast<std::int16_t>(srcX0 + ix0 - px),
                              static_cast<std::int16_t>(srcY0 + iy0 - py),
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
    if (ledsLoaded_)
        displayMirror_addLayer(LT7683::kLedsAddr, stride * 2UL * h, 0, 1,
                               displayMirror_fileAssetId(kLedsBmpPath, stride));
    switchSpriteLoaded_ = ledsLoaded_ && w >= kSwitchOn.srcX + kSwitchOn.w && h >= kSwitchOn.h;
    standbySpriteLoaded_ = ledsLoaded_ && w >= kSwitchStandby.srcX + kSwitchStandby.w &&
                           h >= kSwitchStandby.h;
    LOGF("\r\n\t* %s %ux%u: LEDs %s, switch ON %s, switch STANDBY %s", kLedsBmpPath, w, h,
         ledsLoaded_ ? "ok" : "FAILED",
         switchSpriteLoaded_ ? "ok" : "missing",
         standbySpriteLoaded_ ? "ok" : "missing (leds.bmp older than 240 px wide?)");
}

void loadReels() {
    std::uint16_t w = 0, h = 0;
    if (!peekBmpDimensions(kReelsBmpPath, w, h)) {
        LOGF("\r\n\t* %s not found -- no reel animation", kReelsBmpPath);
        return;
    }
    const std::uint16_t stride = static_cast<std::uint16_t>((w + 3) & ~3);
    if (w < kReelFrames * kReelBox || h < 2 * kReelBox ||
        static_cast<std::uint32_t>(stride) * h * 2 > LT7683::kReelsMaxBytes) {
        LOGF("\r\n\t* %s is %ux%u, expected %ux%u -- no reel animation",
             kReelsBmpPath, w, h, kReelFrames * kReelBox, 2 * kReelBox);
        return;
    }
    {
        LayerDrawScope scope(LT7683::kReelsAddr, stride);
        display_->setActiveWindow(0, 0, stride - 1, SCREEN_MAX_Y - 1);
        reelsLoaded_ = drawBmpAt(display_, kReelsBmpPath, 0, 0);
    }
    display_->setActiveWindow(0, 0, SCREEN_MAX_X - 1, SCREEN_MAX_Y - 1);
    screen_->reassertRenderState();
    reelsStride_ = stride;
    if (reelsLoaded_)
        displayMirror_addLayer(LT7683::kReelsAddr, stride * 2UL * h, 0, 1,
                               displayMirror_fileAssetId(kReelsBmpPath, stride));
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
        // Reels at their current animation angle (frame 0 = the picture)
        if (reelsLoaded_ && reelFrame_ != 0) {
            for (int i = 0; i < 2; ++i) {
                overlayPatch(LT7683::kReelsAddr, reelsStride_,
                             tapeX_ + kCassetteX + kReels[i].x,
                             tapeY_ + kCassetteY + kReels[i].y, kReelBox, kReelBox,
                             x, y, w, h, reelFrame_ * kReelBox, i * kReelBox);
            }
        }
    }
    if (ledsLoaded_) {
        // Switch: OFF is the photo itself; ON and STANDBY are sprites
        for (const auto& [led, lit] : {std::pair{kPowerLed, powerLit_},
                                       std::pair{kBusyLed,  busyLit_},
                                       std::pair{kSwitchOn, switchShown_ == DrivePower::On &&
                                                            switchSpriteLoaded_},
                                       std::pair{kSwitchStandby, switchShown_ == DrivePower::Standby &&
                                                                 standbySpriteLoaded_}}) {
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
        loadReels();
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

void plotterview_showMessage(const char* text) {
    showSplashBox(text, 1, 2500);
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
    if (output_ == DisplayOutput::CharTable) {
        chartable_drawAll();
        return;
    }
    if (output_ == DisplayOutput::Signals) {
        hpil_diag_drawAll();
        return;
    }
    if (output_ == DisplayOutput::LoopMap) {
        loopmap_drawAll();
        return;
    }
    if (output_ == DisplayOutput::Clock) {
        clock_drawAll();
        return;
    }
    if (output_ == DisplayOutput::Analyzer) {
        analyzer_redrawAll();
        return;
    }
    if (output_ == DisplayOutput::Tape) {
        drawTapeRegion(0, 0, SCREEN_MAX_X, SCREEN_MAX_Y);
        return;
    }
    if (output_ != DisplayOutput::Plotter) return;
    // The mapping: the whole page, or (Fit) the plot itself with a margin
    // -- 5 % when fitted afresh here, so it has room to grow a little
    setPageMapping();
    if (plotFit_ && !plotter_->segments().empty()) {
        double x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
        for (const PlotSegment& seg : plotter_->segments()) {
            x0 = std::min(x0, static_cast<double>(std::min(seg.x0, seg.x1)));
            x1 = std::max(x1, static_cast<double>(std::max(seg.x0, seg.x1)));
            y0 = std::min(y0, static_cast<double>(std::min(seg.y0, seg.y1)));
            y1 = std::max(y1, static_cast<double>(std::max(seg.y0, seg.y1)));
        }
        setFitMapping(x0, y0, x1, y1, 0.05);
    }
    plotWindow();
    display_->fillRect(0, 0, SCREEN_MAX_X, SCREEN_MAX_Y, paperColor());
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
        drawMappedSegment(seg.x0, seg.y0, seg.x1, seg.y1, penColor(seg.pen));
    }
    // Restore the full-screen active window Plotter mode normally runs
    // with -- the caller (boardui.cpp's hideButtonStrip()) already leaves
    // it wide at this point, so this just keeps it that way.
    display_->setActiveWindow(0, 0, SCREEN_MAX_X - 1, SCREEN_MAX_Y - 1);
    screen_->refreshCursor();
}

// Switches the panel to `mode`, for device `dev` (its title goes in the
// splash). Tape -> Tape with another drive also counts as a switch.
// splash = false: switch at once, no name box (the screen saver)
static void switchView(DisplayOutput mode, CDevice* dev, bool splash = true) {
    if (!plotterview_isAvailable(mode)) return;
    if (mode == output_ && dev == viewDevice_) return;
    viewDevice_ = dev;
    if (mode == DisplayOutput::Tape && dev != nullptr && dev != drive_) {
        // Another drive: its own cassette, lid and lights
        drive_ = static_cast<CDrive*>(dev);
        tapeFile_ = drive_->mediaFile();
        tapeEjected_ = drive_->ejected();
        powerLit_ = drive_->poweredUp();
        busyLit_ = powerLit_ && drive_->busyLight();
        switchShown_ = drive_->powerMode();
        reelFrame_ = 0;
#ifdef DISPLAY_7INCH
        composeCassetteLabel();
#endif
    }
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
    if (!splash) {
        revealCurrentView();
        return;
    }
    std::string title = dev ? plotterview_viewTitle(dev) : std::string(outputName(mode));
    for (char& c : title) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    showSplashBox(title.c_str(), 2, kSplashMs);
#ifdef DISPLAY_7INCH
    // Draw the new view now, hidden behind the full-screen splash overlay
    // -- it's already complete when plotterview_poll() removes the overlay.
    revealCurrentView();
#endif
    // 5": deliberately do NOT draw the new view (or resume() Screen) here
    // -- the splash is drawn directly on the panel and would be
    // overwritten. plotterview_poll() does it once the splash times out.
}

void plotterview_showDevice(CDevice* dev) {
    if (deviceHasView(dev)) switchView(outputFor(dev->viewKind()), dev);
}

CDevice* plotterview_viewDevice() { return viewDevice_; }

// ── View chosen from the HP-41 ("ESC # V <addr>") ───────────────────────
// -1 = no request. Written from HP-IL frame handling, read from the main
// loop -- same core, so a plain volatile is enough; a newer request
// simply replaces one not yet taken.
static volatile int viewRequest_ = -1;

void plotterview_requestViewByAddress(std::uint8_t addr) { viewRequest_ = addr; }

bool plotterview_takeViewRequest(std::uint8_t& addr) {
    const int r = viewRequest_;
    if (r < 0) return false;
    viewRequest_ = -1;
    addr = static_cast<std::uint8_t>(r);
    return true;
}

bool plotterview_showAddress(std::uint8_t addr) {
    // HP-IL device addresses are 1..30 (31 = not addressed)
    if (addr < 1 || addr > 30) {
        LOGF("\r\n * ESC # V %u: not a device address (1..30) -- ignored", addr);
        return false;
    }
    for (CDevice* dev : devices) {
        // A disabled device has no address (31), so it never matches here
        if (dev == nullptr || !dev->enabled() || dev->addr() != addr) continue;
        if (!deviceHasView(dev)) {
            LOGF("\r\n * ESC # V %u: %s has no view -- ignored", addr, dev->name());
            return false;
        }
        LOGF("\r\n * ESC # V %u: showing %s", addr, plotterview_viewTitle(dev).c_str());
        switchView(outputFor(dev->viewKind()), dev);
        return true;
    }
    LOGF("\r\n * ESC # V %u: no HIPI device at that address -- ignored", addr);
    return false;
}

// The view to go back to when leaving the Analyzer
static CDevice* beforeAnalyzer_ = nullptr;
static DisplayOutput beforeAnalyzerOutput_ = DisplayOutput::Display;

void plotterview_showAnalyzer() {
    if (output_ == DisplayOutput::Analyzer) return;
    beforeAnalyzer_ = viewDevice_;
    beforeAnalyzerOutput_ = output_;
    switchView(DisplayOutput::Analyzer, nullptr);
}

// The view to go back to when leaving the loop map
static CDevice* beforeLoopMap_ = nullptr;
static DisplayOutput beforeLoopMapOutput_ = DisplayOutput::Display;

void plotterview_showLoopMap() {
    if (output_ == DisplayOutput::LoopMap) return;
    beforeLoopMap_ = viewDevice_;
    beforeLoopMapOutput_ = output_;
    switchView(DisplayOutput::LoopMap, nullptr);
}

void plotterview_leaveLoopMap() {
    if (output_ != DisplayOutput::LoopMap) return;
    if (beforeLoopMapOutput_ == DisplayOutput::Analyzer) {
        switchView(DisplayOutput::Analyzer, nullptr);
    } else if (deviceHasView(beforeLoopMap_)) {
        switchView(outputFor(beforeLoopMap_->viewKind()), beforeLoopMap_);
    } else {
        switchView(DisplayOutput::Display, nullptr);
    }
}

// The view to go back to when leaving the HP-IL signals view
static CDevice* beforeSignals_ = nullptr;
static DisplayOutput beforeSignalsOutput_ = DisplayOutput::Display;

void plotterview_showSignals() {
    if (output_ == DisplayOutput::Signals) return;
    beforeSignals_ = viewDevice_;
    beforeSignalsOutput_ = output_;
    switchView(DisplayOutput::Signals, nullptr);
}

void plotterview_leaveSignals() {
    if (output_ != DisplayOutput::Signals) return;
    if (beforeSignalsOutput_ == DisplayOutput::Analyzer || beforeSignalsOutput_ == DisplayOutput::LoopMap) {
        switchView(beforeSignalsOutput_, nullptr);
    } else if (deviceHasView(beforeSignals_)) {
        switchView(outputFor(beforeSignals_->viewKind()), beforeSignals_);
    } else {
        switchView(DisplayOutput::Display, nullptr);
    }
}

// The view to go back to when the character table closes
static CDevice* beforeCharTable_ = nullptr;
static DisplayOutput beforeCharTableOutput_ = DisplayOutput::Display;

void plotterview_showCharTable() {
    if (output_ == DisplayOutput::CharTable) return;
    beforeCharTable_ = viewDevice_;
    beforeCharTableOutput_ = output_;
    switchView(DisplayOutput::CharTable, nullptr, /*splash=*/false);
}

void plotterview_leaveCharTable() {
    if (output_ != DisplayOutput::CharTable) return;
    const DisplayOutput o = beforeCharTableOutput_;
    if (o == DisplayOutput::Analyzer || o == DisplayOutput::LoopMap || o == DisplayOutput::Signals) {
        switchView(o, nullptr, false);
    } else if (deviceHasView(beforeCharTable_)) {
        switchView(outputFor(beforeCharTable_->viewKind()), beforeCharTable_, false);
    } else {
        switchView(DisplayOutput::Display, nullptr, false);
    }
}

// The view to go back to when the clock screen saver ends
static CDevice* beforeClock_ = nullptr;
static DisplayOutput beforeClockOutput_ = DisplayOutput::Display;

void plotterview_showClock() {
    if (output_ == DisplayOutput::Clock) return;
    beforeClock_ = viewDevice_;
    beforeClockOutput_ = output_;
    switchView(DisplayOutput::Clock, nullptr, /*splash=*/false);
}

void plotterview_leaveClock() {
    if (output_ != DisplayOutput::Clock) return;
    if (beforeClockOutput_ == DisplayOutput::Analyzer) {
        switchView(DisplayOutput::Analyzer, nullptr, false);
    } else if (deviceHasView(beforeClock_)) {
        switchView(outputFor(beforeClock_->viewKind()), beforeClock_, false);
    } else {
        switchView(DisplayOutput::Display, nullptr, false);
    }
}

void plotterview_leaveAnalyzer() {
    if (output_ != DisplayOutput::Analyzer) return;
    if (deviceHasView(beforeAnalyzer_)) {
        switchView(outputFor(beforeAnalyzer_->viewKind()), beforeAnalyzer_);
    } else {
        switchView(beforeAnalyzerOutput_ == DisplayOutput::Tape ? DisplayOutput::Display
                                                                  : beforeAnalyzerOutput_, nullptr);
    }
}

std::vector<CDevice*> plotterview_viewDevices() {
    std::vector<CDevice*> list;
    for (CDevice* dev : devices) {
        if (deviceHasView(dev)) list.push_back(dev);
    }
    return list;
}

std::string plotterview_viewTitle(CDevice* dev) {
    std::string title;
    switch (dev->viewKind()) {
        case ViewKind::Display: title = "Display"; break;
        case ViewKind::Plotter: title = "Plotter"; break;
        case ViewKind::Tape:    title = "Tape";    break;
        default:                title = dev->name(); return title;
    }
    // Several devices with the same kind of view: add the device name
    int same = 0;
    for (CDevice* d : devices) {
        if (deviceHasView(d) && d->viewKind() == dev->viewKind()) ++same;
    }
    if (same > 1) {
        title += " ";
        title += dev->name();
    }
    return title;
}

bool plotterview_isSplashVisible() { return splashVisible_; }

void plotterview_poll() {
    // Status LEDs -- redraw only on change (a single BTE copy each)
    if (ledsLoaded_ && drive_ != nullptr) {
        const bool power = drive_->poweredUp();        // POWER light
        const bool busy  = power && drive_->busyLight();   // BUSY light, per the manual
        const DrivePower sw = drive_->powerMode();       // switch position
        if (power != powerLit_) {
            powerLit_ = power;
            refreshLed(kPowerLed);
        }
        if (sw != switchShown_) {
            switchShown_ = sw;
            refreshLed(kSwitchOn);           // same area for all positions
        }
        if (busy  != busyLit_)  { busyLit_  = busy;  refreshLed(kBusyLed); backlight_activity(); }
    }
    // Spinning reels while the drive works (BUSY) -- one frame step per
    // kReelFrameMs, only the two small hub squares are redrawn
    if (reelsLoaded_ && drive_ != nullptr && drive_->isBusy() &&
        output_ == DisplayOutput::Tape && cassetteShown() &&
        time_reached(reelNext_)) {
        // Rewind (REWIND button): faster, and the reels turn the other way
        const bool rewinding = drive_->isRewinding();
        reelNext_ = make_timeout_time_ms(rewinding ? kReelRewindFrameMs : kReelFrameMs);
        reelFrame_ = (reelFrame_ + (rewinding ? kReelFrames - 1 : 1)) % kReelFrames;
        for (const ReelPos& r : kReels) {
            drawTapeRegion(static_cast<std::int16_t>(tapeX_ + kCassetteX + r.x),
                           static_cast<std::int16_t>(tapeY_ + kCassetteY + r.y),
                           kReelBox, kReelBox);
        }
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
    // Step through the devices in LOOP ORDER (forward = next on the loop)
    // to the next one that has a view. Devices without a view, switched
    // off, or whose view can't be shown here are skipped.
    const int n = static_cast<int>(devices.size());
    if (n == 0) return;
    int start = -1;
    for (int i = 0; i < n; ++i) {
        if (devices[static_cast<std::size_t>(i)] == viewDevice_) { start = i; break; }
    }
    if (start < 0) {
        // Not on a device's view yet (e.g. right after start-up): start
        // from the first device showing the current kind of view
        for (int i = 0; i < n; ++i) {
            CDevice* d = devices[static_cast<std::size_t>(i)];
            if (deviceHasView(d) && outputFor(d->viewKind()) == output_) { start = i; break; }
        }
        if (start < 0) start = forward ? n - 1 : 0;
    }
    const int step = forward ? 1 : -1;
    for (int k = 1; k <= n; ++k) {
        CDevice* d = devices[static_cast<std::size_t>(((start + k * step) % n + n) % n)];
        if (deviceHasView(d)) {
            switchView(outputFor(d->viewKind()), d);
            return;
        }
    }
    // No device has a view at all: the text display is always there
    switchView(DisplayOutput::Display, nullptr);
}

void plotterview_setPlotFit(bool fit) {
    plotFit_ = fit;
    if (output_ == DisplayOutput::Plotter) plotterview_redraw();
}
bool plotterview_plotFit() { return plotFit_; }

void plotterview_setPaperBlack(bool black) {
    paperBlack_ = black;
    if (output_ == DisplayOutput::Plotter) plotterview_redraw();
}
bool plotterview_paperBlack() { return paperBlack_; }

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
