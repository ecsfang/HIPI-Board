#include "views.h"
#include "views_impl.h"
#include "sd_paths.h"
#include "boardui.h"
#include "usb_serial.h"
#include "bmp_loader.hpp"
#include "display_mirror.h"   // layers a PC viewer needs (tools/hipiview)
#include "drive.h"
#include "printer.h"
#include "video80.h"
#include "analyzer.h"
#include "clock.h"
#include "loopmap.h"
#include "hwtest.h"
#include "mediamap.h"
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

namespace views_impl {
DisplayDriver* display_ = nullptr;
Screen* screen_ = nullptr;
DisplayOutput output_ = DisplayOutput::Display;
}  // namespace views_impl

using namespace views_impl;

namespace {

// The device whose view is showing (nullptr = none yet / the Display view
// with no display device) -- swiping steps through the devices from here
CDevice* viewDevice_ = nullptr;
// The display's HP 92198 screen (video80.h) -- it draws itself. The
// Display view is it (output Video80) while TFDISPLAY emulates the 92198,
// else Screen (output Display).
Video80* video80_ = nullptr;
bool displayV80_ = false;
DisplayOutput displayOutput() {
    return displayV80_ && video80_ != nullptr ? DisplayOutput::Video80 : DisplayOutput::Display;
}

// ── View-switch splash ──────────────────────────────────────────────────
// Briefly announces which view is now showing, whenever it actually
// changes (menu-driven or via the swipe gesture) -- non-blocking: drawn
// once here, then dismissed later by views_poll() once the
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
        case DisplayOutput::HwTest:   return "HARDWARE TEST";
        case DisplayOutput::MediaMap: return "MEDIA MAP";
        case DisplayOutput::Printer:  return "PRINTER";
        case DisplayOutput::Video80:  return "DISPLAY";     // (the HP 92198)
    }
    return "?";
}

DisplayOutput outputFor(ViewKind k) {
    switch (k) {
        case ViewKind::Plotter: return DisplayOutput::Plotter;
        case ViewKind::Tape:    return DisplayOutput::Tape;
        case ViewKind::Printer: return DisplayOutput::Printer;
        default:                return displayOutput();
    }
}

// On the loop, switched on, and its kind of view can be shown here. The
// printer's view is there even while it's switched off: it starts off
// (hipi_init()), and its power switch is in the view.
bool deviceHasView(CDevice* dev) {
    return dev != nullptr && (dev->enabled() || dev->viewKind() == ViewKind::Printer) &&
           dev->viewKind() != ViewKind::None &&
           views_isAvailable(outputFor(dev->viewKind()));
}

// ─── Tape view (7" panel only) ──────────────────────────────────────────
// hp82161a.bmp is drawn ONCE at boot into its own off-screen SDRAM layer
// (LT7683::kTapeLayerAddr), centered and surrounded by black. Showing the
// view, or restoring a part of it (e.g. after the button strip hides), is
// then a single in-chip BTE copy -- no SD card or SPI pixel traffic.
constexpr const char* kTapeBmpPath = HIPI_PATH(HIPI_DIR_RESOURCES, "hp82161a.bmp");
bool tapeLoaded_ = false;
std::int16_t tapeX_ = 0, tapeY_ = 0;     // image's top-left on screen
std::uint16_t tapeW_ = 0, tapeH_ = 0;    // image size

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
std::uint16_t lidOpenStride_ = 0;

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
bool switchSpriteLoaded_ = false;   // leds.bmp has the ON switch
bool standbySpriteLoaded_ = false;  // ... and the STANDBY switch
DrivePower switchShown_ = DrivePower::Off;           // switch position currently drawn
bool ledsLoaded_ = false;
std::uint16_t ledsStride_ = 0;
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
std::uint16_t reelsStride_ = 0;
int reelFrame_ = 0;
bool demoTape_ = false;           // the demo: the drive shown working (views_setDemoTape())
absolute_time_t reelNext_ = nil_time;

bool cassetteLoaded_ = false;
std::uint16_t cassetteW_ = 0, cassetteH_ = 0;
std::string tapeFile_;          // selected .dat file, "" = none
bool tapeEjected_ = false;      // true while the file picker is open

bool cassetteShown() {
    return cassetteLoaded_ && !tapeFile_.empty() && !tapeEjected_;
}

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

void drawTapeRegion(std::int16_t x, std::int16_t y, std::int16_t w, std::int16_t h) {
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
}

// One reel hub at its current frame, in ONE copy to the panel: the frame
// from reels.bmp, or (frame 0) the hub as the cassette picture has it.
// Redrawing it with drawTapeRegion() instead puts the drive's photo there
// first (the bare pins: black squares), then the cassette, then the frame
// -- three copies onto the visible panel, and now and then the panel shows
// the first one for a moment. The hub's box is opaque and inside the
// cassette, nothing else is drawn over it, so one copy gives the same
// picture. (A box not wholly inside the cassette: drawTapeRegion().)
void drawReel(int i) {
    const ReelPos& r = kReels[i];
    const int x = tapeX_ + kCassetteX + r.x, y = tapeY_ + kCassetteY + r.y;
    if (r.x < 0 || r.y < 0 || r.x + kReelBox > cassetteW_ || r.y + kReelBox > cassetteH_) {
        drawTapeRegion(static_cast<std::int16_t>(x), static_cast<std::int16_t>(y), kReelBox, kReelBox);
        return;
    }
    display_->setActiveWindow(0, 0, SCREEN_MAX_X - 1, SCREEN_MAX_Y - 1);
    if (reelFrame_ != 0)
        overlayPatch(LT7683::kReelsAddr, reelsStride_, x, y, kReelBox, kReelBox,
                     x, y, kReelBox, kReelBox, reelFrame_ * kReelBox, i * kReelBox);
    else
        overlayPatch(LT7683::kCassetteLabelAddr, SCREEN_MAX_X,
                     tapeX_ + kCassetteX, tapeY_ + kCassetteY, cassetteW_, cassetteH_,
                     x, y, kReelBox, kReelBox);
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

// ─── Printer view (7" panel only) ───────────────────────────────────────
// hp82162a.bmp -- the HP82162A, made to match hp82161a.bmp in size, place
// and colour (the two devices are the same size and stack) -- is drawn
// once at boot into its own layer (LT7683::kPrinterLayerAddr), like the
// Tape view. prtparts.bmp holds its moving parts, each cut from the same
// picture with that one control changed: the POWER light lit, the power
// switch ON, the MODE switch in each position, the INTENSITY switch in
// each position, PRINT and PAPER ADVANCE pressed. The picture itself shows
// power OFF, MAN, intensity in the middle, keys up.
// Coordinates are in hp82162a.bmp's own pixels (tools/images/
// make_hp82162a.py prints the sprite table) -- re-measure if it changes.
constexpr const char* kPrinterBmpPath = HIPI_PATH(HIPI_DIR_RESOURCES, "hp82162a.bmp");
constexpr const char* kPrinterPartsPath = HIPI_PATH(HIPI_DIR_RESOURCES, "prtparts.bmp");
bool printerLoaded_ = false;
bool printerPartsLoaded_ = false;
std::int16_t printerX_ = 0, printerY_ = 0;           // picture's top-left on screen
std::uint16_t printerPartsStride_ = 0;
CPrinter* printer_ = nullptr;

struct Sprite { std::int16_t x, y, w, h, srcX; };   // picture rect, column in prtparts.bmp
constexpr Sprite kPrtPowerLed  {120, 484, 40, 40,   0};
constexpr Sprite kPrtPowerOn   {138, 436, 64, 32,  40};
constexpr Sprite kPrtMode[3]   {{289, 496, 84, 32, 104}, {289, 496, 84, 32, 188}, {289, 496, 84, 32, 272}};
constexpr Sprite kPrtIntensity[5] {{289, 435, 84, 32, 356}, {289, 435, 84, 32, 440}, {289, 435, 84, 32, 524},
                                   {289, 435, 84, 32, 608}, {289, 435, 84, 32, 692}};
constexpr Sprite kPrtPrintDown {496, 439, 76, 52, 776};
constexpr Sprite kPrtAdvDown   {652, 441, 76, 52, 852};
// The print head, as photographed: parked on the right in the slot just
// under the paper (the picture has the slot without it). x moves.
constexpr Sprite kPrtHead      {683, 347, 66, 11, 928};
constexpr int kPrtBaseMode = 0, kPrtBaseIntensity = 2;     // as in the picture itself
constexpr std::int16_t kPrtPartsW = 996, kPrtPartsH = 52;
// Knob centres (picture x) of the 5- and 3-position switches
constexpr int kPrtKnob0X = 309, kPrtIntensityStep = 11, kPrtModeStep = 22;

// The front panel as last drawn
bool prtPowerShown_ = false;
int prtModeShown_ = kPrtBaseMode, prtIntensityShown_ = kPrtBaseIntensity;
bool prtPrintShown_ = false, prtAdvShown_ = false;
// A key is shown pressed at least this long (a quick tap is still seen)
constexpr std::uint32_t kPrtKeyShowMs = 150;
absolute_time_t prtPrintUntil_ = nil_time, prtAdvUntil_ = nil_time;

void loadPrinterLayer() {
    std::uint16_t w = 0, h = 0;
    if (!peekBmpDimensions(kPrinterBmpPath, w, h)) {
        LOGF("\r\n\t* %s not found -- Printer view disabled", kPrinterBmpPath);
        return;
    }
    if (w > SCREEN_MAX_X || h > SCREEN_MAX_Y) {
        LOGF("\r\n\t* %s is %ux%u, larger than the panel -- Printer view disabled",
             kPrinterBmpPath, w, h);
        return;
    }
    printerX_ = static_cast<std::int16_t>((SCREEN_MAX_X - w) / 2);
    printerY_ = static_cast<std::int16_t>((SCREEN_MAX_Y - h) / 2);
    display_->setActiveWindow(0, 0, SCREEN_MAX_X - 1, SCREEN_MAX_Y - 1);
    display_->beginLayerDraw(LT7683::kPrinterLayerAddr);
    display_->fillRect(0, 0, SCREEN_MAX_X, SCREEN_MAX_Y, 0x0000);
    printerLoaded_ = drawBmpAt(display_, kPrinterBmpPath, printerX_, printerY_);
    display_->endOverlayDraw();
    if (printerLoaded_)
        displayMirror_addLayer(LT7683::kPrinterLayerAddr, SCREEN_MAX_X * 2UL * SCREEN_MAX_Y, 0, 1,
                               displayMirror_fileAssetId(kPrinterBmpPath, static_cast<std::uint32_t>(printerX_) | (static_cast<std::uint32_t>(printerY_) << 16)));
    screen_->reassertRenderState();
    LOGF("\r\n\t* Printer view %s", printerLoaded_ ? "ready" : "disabled (bad BMP)");
}

void loadPrinterParts() {
    std::uint16_t w = 0, h = 0;
    if (!peekBmpDimensions(kPrinterPartsPath, w, h)) {
        LOGF("\r\n\t* %s not found -- the Printer view's controls don't move", kPrinterPartsPath);
        return;
    }
    const std::uint16_t stride = static_cast<std::uint16_t>((w + 3) & ~3);
    if (w < kPrtPartsW || h < kPrtPartsH ||
        static_cast<std::uint32_t>(stride) * h * 2 > LT7683::kPrinterPartsMaxBytes) {
        LOGF("\r\n\t* %s is %ux%u, expected %ux%u -- the Printer view's controls don't move",
             kPrinterPartsPath, w, h, kPrtPartsW, kPrtPartsH);
        return;
    }
    {
        LayerDrawScope scope(LT7683::kPrinterPartsAddr, stride);
        display_->setActiveWindow(0, 0, stride - 1, SCREEN_MAX_Y - 1);
        printerPartsLoaded_ = drawBmpAt(display_, kPrinterPartsPath, 0, 0);
    }
    display_->setActiveWindow(0, 0, SCREEN_MAX_X - 1, SCREEN_MAX_Y - 1);
    screen_->reassertRenderState();
    printerPartsStride_ = stride;
    if (printerPartsLoaded_)
        displayMirror_addLayer(LT7683::kPrinterPartsAddr, stride * 2UL * h, 0, 1,
                               displayMirror_fileAssetId(kPrinterPartsPath, stride));
}

void overlayInk(int x, int y, int w, int h);   // (the paper, below)
void overlayHead(int x, int y, int w, int h);  // (the print head, below)

void drawPrinterRegion(std::int16_t x, std::int16_t y, std::int16_t w, std::int16_t h) {
    display_->setActiveWindow(0, 0, SCREEN_MAX_X - 1, SCREEN_MAX_Y - 1);
    display_->copyLayerToPanel(LT7683::kPrinterLayerAddr, x, y,
                               static_cast<std::uint16_t>(w), static_cast<std::uint16_t>(h));
    overlayInk(x, y, w, h);                    // what's printed, on the paper
    overlayHead(x, y, w, h);                   // and the print head
    if (printerPartsLoaded_) {
        auto overlay = [&](const Sprite& s) {
            overlayPatch(LT7683::kPrinterPartsAddr, printerPartsStride_,
                         printerX_ + s.x, printerY_ + s.y, s.w, s.h, x, y, w, h, s.srcX);
        };
        if (prtPowerShown_) { overlay(kPrtPowerOn); overlay(kPrtPowerLed); }
        if (prtModeShown_ != kPrtBaseMode) overlay(kPrtMode[prtModeShown_]);
        if (prtIntensityShown_ != kPrtBaseIntensity) overlay(kPrtIntensity[prtIntensityShown_]);
        if (prtPrintShown_) overlay(kPrtPrintDown);
        if (prtAdvShown_) overlay(kPrtAdvDown);
    }
    screen_->refreshCursor();   // BTE left the chip in graphics mode
}

// Redraws one control's area if the Printer view is showing
void refreshPrinterPart(const Sprite& s) {
    if (output_ != DisplayOutput::Printer) return;
    drawPrinterRegion(static_cast<std::int16_t>(printerX_ + s.x),
                      static_cast<std::int16_t>(printerY_ + s.y), s.w, s.h);
}

// Brings the drawn front panel in line with the printer -- only what
// changed is redrawn (a BTE copy each)
void pollPrinterPanel() {
    if (printer_ == nullptr || !printerLoaded_) return;
    const bool power = printer_->enabled();
    const int mode = static_cast<int>(printer_->mode());
    const int intensity = printer_->intensity();
    const bool prt = printer_->printDown() || !time_reached(prtPrintUntil_);
    const bool adv = printer_->advanceDown() || !time_reached(prtAdvUntil_);
    if (power != prtPowerShown_) {
        prtPowerShown_ = power;
        refreshPrinterPart(kPrtPowerOn);
        refreshPrinterPart(kPrtPowerLed);
    }
    if (mode != prtModeShown_ && mode >= 0 && mode < 3) {
        prtModeShown_ = mode;
        refreshPrinterPart(kPrtMode[0]);           // the same area for all positions
    }
    if (intensity != prtIntensityShown_ && intensity >= 0 && intensity < CPrinter::kIntensities) {
        prtIntensityShown_ = intensity;
        refreshPrinterPart(kPrtIntensity[0]);
    }
    if (prt != prtPrintShown_) { prtPrintShown_ = prt; refreshPrinterPart(kPrtPrintDown); }
    if (adv != prtAdvShown_)   { prtAdvShown_ = adv;   refreshPrinterPart(kPrtAdvDown); }
}

// ─── The paper in the Printer view (7" only) ────────────────────────────
// What has been printed is kept as ink on white in its own layer
// (LT7683::kPrinterInkAddr): kInkW x kInkH pixels, the print area seen
// through the window, the newest line at the bottom (at the print head).
// On the panel it's combined with the picture's own paper -- BTE "S0 AND
// destination" (LT7683::andLayerRegion()) -- so the shade under the
// cover stays; the tear-off bar (kBarY0..kBarY1) is left out, the paper
// passes behind it.
//
// Dots: the printer's 168 dot columns at 1.5 pixels each = 252 pixels
// (the paper in the picture is 280 wide), anti-aliased, dark blue-black,
// as dark as the INTENSITY switch says. A line is 16 pixels: 8 dot rows
// (12 px) and a gap.
//
// A new line, as on the real printer: the print head -- barely visible in
// the slot under the paper -- glides over to the other side (kGlideSteps
// steps), printing where it can't be seen, then the paper moves up one
// line (kFeedSteps steps) and the line comes into view. The next line the
// head glides back, and so on. A blank line (PAPER ADVANCE, an empty CR)
// only moves the paper. The ink layer has one line more than the window
// shows, at the bottom (kInkBufH): that's where a line is printed before
// the paper brings it up. One step per views_poll() at most every
// kGlideStepMs / kFeedStepMs, never holding up the main loop. Lines
// arriving faster than that go onto the paper at once, without the
// animation (the head jumps to where it would end up); so do lines
// printed while the view isn't showing.
constexpr int kPaperY0 = 24, kPaperY1 = 347;               // paper rows in the window (picture)
constexpr int kBarY0 = 290, kBarY1 = 310;                  // tear-off bar
constexpr int kInkW = 252, kInkH = kPaperY1 - kPaperY0;    // 168 dots x 1.5
constexpr int kPaperX0 = 482, kPaperW = 280;               // the paper in the window (picture)
constexpr int kInkX = kPaperX0 + (kPaperW - kInkW) / 2;    // print area left
constexpr int kLinePx = 16, kDotRowsPx = 12, kLineTopPx = 2;
constexpr int kInkBufH = kInkH + kLinePx;                  // + the line not yet in view
// Below the tear-off bar the paper bends towards the print head and runs
// a little to the left: row kBarY1 + i is moved left by kSlant[i] pixels,
// in the picture (tools/images/make_hp82162a.py prints this table) and so
// the ink on it too.
constexpr std::uint8_t kSlant[kPaperY1 - kBarY1] = {
    0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 3, 3, 3,
    3, 3, 4, 4, 4, 5, 5, 5, 5, 6, 6, 6, 7, 7, 7, 8, 8 };
constexpr int kFeedSteps = 4, kGlideSteps = 12;
constexpr std::uint32_t kFeedStepMs = 20, kGlideStepMs = 15;
// The head's travel (left edge of its sprite, picture x): from the
// window's left side to where it's parked in the photo
constexpr int kHeadMinX = kPaperX0 + 1, kHeadMaxX = kPrtHead.x;
constexpr std::uint32_t kAdvanceRepeatMs = 220;             // PAPER ADVANCE held
static_assert(static_cast<std::uint32_t>(kInkW) * kInkBufH * 2 <= LT7683::kPrinterInkMaxBytes, "ink layer");

bool inkReady_ = false;
std::uint32_t paperShown_ = 0;            // lines on the paper: those numbered < this
enum class PaperPhase { Idle, Glide, Feed } paperPhase_ = PaperPhase::Idle;
int paperStep_ = 0;
absolute_time_t paperNext_ = nil_time;
absolute_time_t advanceRepeat_ = nil_time;
int headX_ = kHeadMaxX;                   // the head's place (sprite left edge, picture x)
int headFrom_ = kHeadMaxX;                // a glide: from ...
int headTo_ = kHeadMinX;                  // ... to
bool paperDirty_ = false;                 // redraw it all (too much to catch up)
// Moved by hand (views_paperDragBegin()): how far the paper is
// from where it prints, in pixels -- > 0 down (older lines in view),
// < 0 pulled up (the newest line out above the tear-off bar)
int paperOff_ = 0;
bool paperDrag_ = false;
int dragY0_ = 0, dragOff0_ = 0;
// Held still after dragging: the paper goes on by itself in the drag's
// direction (pollPaper()) until the finger moves or lets go, or the end
// is reached -- a line every kAutoStepMs, faster the longer it's held
int holdY_ = 0;                           // where the finger stopped ...
absolute_time_t holdSince_ = nil_time;    // ... and since when
int dragDir_ = 0;                         // the last drag's direction: 1 down, -1 up, 0 none yet
bool autoScroll_ = false;
absolute_time_t autoStart_ = nil_time, autoNext_ = nil_time;
constexpr int kHoldSlop = 6;              // px: less is holding still
constexpr int kHoldMs = 300;              // still this long: it goes on
constexpr int kAutoStepMs = 60;
bool paperReturn_ = false;                // gliding back to the print head
absolute_time_t returnNext_ = nil_time;
constexpr int kReturnStepMs = 15;
// Pulled up: the newest line's bottom just above the tear-off bar
constexpr int kMaxPullUp = kInkH - (kBarY0 - kPaperY0) + 2;
// Tearing off (views_takeTearGesture()): at the edge, then sideways
constexpr int kTearDx = 100, kTearMaxDy = 80;
bool atEdge_ = false;
int edgeX_ = 0, edgeY_ = 0;
int tearGesture_ = 0;                     // seen in this drag: -1 / 1
int tearDir_ = 0;                         // tearing off: sliding away to -1 / 1
int tearStep_ = 0;
absolute_time_t tearNext_ = nil_time;
constexpr int kTearSteps = 12, kTearStepMs = 18;

// Ink colour at full darkness, and how dark each INTENSITY position prints
constexpr int kInkR = 34, kInkG = 38, kInkB = 70;
constexpr float kIntensityDark[CPrinter::kIntensities] = { 0.45f, 0.6f, 0.75f, 0.88f, 1.0f };

// The line being put on the paper, as pixels (buildInkLine())
std::uint16_t inkLine_[kInkW * kLinePx];

// Renders printed line `line` (nullptr: blank) into inkLine_
void buildInkLine(const CPrinter::Line* line) {
    std::uint16_t* buf = inkLine_;
    const int dots = CPrinter::kDots;
    for (int i = 0; i < kInkW * kLinePx; ++i) buf[i] = 0xFFFF;
    if (line != nullptr && printer_ != nullptr) {
        const float dark = kIntensityDark[printer_->intensity() % CPrinter::kIntensities];
        // 1.5 px per dot: pixel 3m is dot 2m, 3m+2 is dot 2m+1, 3m+1 half of each
        auto weights = [](int p, int& d0, float& w0, int& d1, float& w1) {
            const int m = p / 3, r = p % 3;
            d0 = 2 * m + (r == 2 ? 1 : 0); w0 = (r == 1) ? 0.5f : 1.0f;
            d1 = 2 * m + 1;                 w1 = (r == 1) ? 0.5f : 0.0f;
        };
        const int maxDots = dots;
        for (int px = 0; px < kInkW; ++px) {
            int dx0, dx1; float wx0, wx1;
            weights(px, dx0, wx0, dx1, wx1);
            if (dx0 >= maxDots) break;
            const std::uint8_t c0 = line->dots[static_cast<std::size_t>(dx0)];
            const std::uint8_t c1 = (wx1 > 0 && dx1 < maxDots) ? line->dots[static_cast<std::size_t>(dx1)] : 0;
            if ((c0 | c1) == 0) continue;
            for (int py = 0; py < kDotRowsPx; ++py) {
                int dy0, dy1; float wy0, wy1;
                weights(py, dy0, wy0, dy1, wy1);
                float cov = 0;
                if (c0 >> dy0 & 1) cov += wx0 * wy0;
                if (wy1 > 0 && (c0 >> dy1 & 1)) cov += wx0 * wy1;
                if (wx1 > 0) {
                    if (c1 >> dy0 & 1) cov += wx1 * wy0;
                    if (wy1 > 0 && (c1 >> dy1 & 1)) cov += wx1 * wy1;
                }
                if (cov <= 0) continue;
                const float k = cov * dark;
                const int r = 255 - static_cast<int>(k * (255 - kInkR));
                const int g = 255 - static_cast<int>(k * (255 - kInkG));
                const int b = 255 - static_cast<int>(k * (255 - kInkB));
                buf[(kLineTopPx + py) * kInkW + px] =
                    static_cast<std::uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
            }
        }
    }
}

// Puts pixel columns x0..x1 of inkLine_ into the ink layer, its top at
// row `row` (may be partly above the layer)
void drawInkSlice(int row, int x0, int x1) {
    x0 = std::max(x0, 0);
    x1 = std::min(x1, kInkW);
    int skip = 0, rows = kLinePx;
    if (row < 0) { skip = -row; rows -= skip; row = 0; }
    if (row + rows > kInkBufH) rows = kInkBufH - row;
    if (rows <= 0 || x1 <= x0) return;
    {
        LayerDrawScope scope(LT7683::kPrinterInkAddr, kInkW);
        display_->setActiveWindow(0, 0, kInkW - 1, SCREEN_MAX_Y - 1);
        display_->drawBitmap565Cropped(static_cast<std::int16_t>(x0), static_cast<std::int16_t>(row),
                                       static_cast<std::uint16_t>(x1 - x0), static_cast<std::uint16_t>(rows),
                                       kInkW, inkLine_ + skip * kInkW + x0);
    }
    display_->setActiveWindow(0, 0, SCREEN_MAX_X - 1, SCREEN_MAX_Y - 1);
    screen_->reassertRenderState();
}

// A whole line at once
void renderInkLine(int row, const CPrinter::Line* line) {
    buildInkLine(line);
    drawInkSlice(row, 0, kInkW);
}

bool blankLine(const CPrinter::Line* l) {
    return l == nullptr || std::all_of(l->dots.begin(), l->dots.end(), [](std::uint8_t d) { return d == 0; });
}

// Draws the print head if it's in the screen region x/y/w/h (the picture
// must already be there)
void overlayHead(int x, int y, int w, int h) {
    if (!inkReady_ || !printerPartsLoaded_) return;
    overlayPatch(LT7683::kPrinterPartsAddr, printerPartsStride_,
                 printerX_ + headX_, printerY_ + kPrtHead.y, kPrtHead.w, kPrtHead.h,
                 x, y, w, h, kPrtHead.srcX);
}

// Redraws the slot between picture x0 and x1 (the head moving there)
void showHeadArea(int x0, int x1) {
    if (output_ != DisplayOutput::Printer) return;
    drawPrinterRegion(static_cast<std::int16_t>(printerX_ + x0), static_cast<std::int16_t>(printerY_ + kPrtHead.y),
                      static_cast<std::int16_t>(x1 - x0 + kPrtHead.w), kPrtHead.h);
}

// The oldest line still kept (the printer keeps the last
// CPrinter::paperCapacity(); tearing off drops them all)
std::uint32_t firstKept() { return printer_->firstKept(); }
// How far down the paper can go: the first line at the top of the window
int maxPullDown() {
    const std::uint32_t first = firstKept();
    const int shown = paperShown_ > first ? static_cast<int>(paperShown_ - first) : 0;
    return std::max(0, shown * kLinePx - kInkH);
}

// Ink layer rows y0..y1 drawn afresh: white, and the lines that lie there
// (line n's top is at kInkH - (paperShown_ - n) * kLinePx + paperOff_)
void renderInkRows(int y0, int y1) {
    y0 = std::max(y0, 0);
    y1 = std::min(y1, kInkBufH);
    if (y1 <= y0) return;
    {
        LayerDrawScope scope(LT7683::kPrinterInkAddr, kInkW);
        display_->setActiveWindow(0, 0, kInkW - 1, SCREEN_MAX_Y - 1);
        display_->fillRect(0, static_cast<std::int16_t>(y0), kInkW, static_cast<std::int16_t>(y1 - y0), 0xFFFF);
    }
    display_->setActiveWindow(0, 0, SCREEN_MAX_X - 1, SCREEN_MAX_Y - 1);
    screen_->reassertRenderState();
    if (printer_ == nullptr) return;
    for (std::uint32_t n = firstKept(); n < paperShown_; ++n) {
        const int top = kInkH - static_cast<int>(paperShown_ - n) * kLinePx + paperOff_;
        if (top + kLinePx <= y0 || top >= y1) continue;
        const CPrinter::Line* l = printer_->line(n);
        if (!blankLine(l)) renderInkLine(top, l);
    }
}

// Moves what's on the paper by dy pixel rows (> 0 down, < 0 up) and draws
// the rows that come into view
void shiftInk(int dy) {
    if (dy == 0) return;
    const int n = std::abs(dy);
    if (n >= kInkBufH) { renderInkRows(0, kInkBufH); return; }
    // Via the staging area: source and destination overlap otherwise
    const std::int16_t from = static_cast<std::int16_t>(dy < 0 ? n : 0), to = static_cast<std::int16_t>(dy < 0 ? 0 : n);
    display_->copyLayerRegion(LT7683::kPrinterInkAddr, kInkW, 0, from,
                              LT7683::kPrinterInkStageAddr, kInkW, 0, 0,
                              kInkW, static_cast<std::uint16_t>(kInkBufH - n));
    display_->copyLayerRegion(LT7683::kPrinterInkStageAddr, kInkW, 0, 0,
                              LT7683::kPrinterInkAddr, kInkW, 0, to,
                              kInkW, static_cast<std::uint16_t>(kInkBufH - n));
    screen_->reassertRenderState();
    if (dy < 0) renderInkRows(kInkBufH - n, kInkBufH);
    else renderInkRows(0, n);
}

// Moves the paper up by n pixel rows; white paper comes in at the bottom
// (printing: the paper is where it prints, nothing below the newest line)
void feedInk(int n) {
    if (n <= 0) return;
    if (n < kInkBufH) {
        display_->copyLayerRegion(LT7683::kPrinterInkAddr, kInkW, 0, static_cast<std::int16_t>(n),
                                  LT7683::kPrinterInkStageAddr, kInkW, 0, 0,
                                  kInkW, static_cast<std::uint16_t>(kInkBufH - n));
        display_->copyLayerRegion(LT7683::kPrinterInkStageAddr, kInkW, 0, 0,
                                  LT7683::kPrinterInkAddr, kInkW, 0, 0,
                                  kInkW, static_cast<std::uint16_t>(kInkBufH - n));
    }
    const int keep = std::max(0, kInkBufH - n);
    {
        LayerDrawScope scope(LT7683::kPrinterInkAddr, kInkW);
        display_->setActiveWindow(0, 0, kInkW - 1, SCREEN_MAX_Y - 1);
        display_->fillRect(0, static_cast<std::int16_t>(keep), kInkW, static_cast<std::int16_t>(kInkBufH - keep), 0xFFFF);
    }
    display_->setActiveWindow(0, 0, SCREEN_MAX_X - 1, SCREEN_MAX_Y - 1);
    screen_->reassertRenderState();
}

// Draws the whole paper again: the lines printed, the newest at the print
// head (or where the paper has been moved to)
void redrawInk() {
    paperShown_ = printer_ != nullptr ? printer_->lineCount() : 0;
    paperOff_ = std::clamp(paperOff_, -kMaxPullUp, maxPullDown());
    renderInkRows(0, kInkBufH);
}

// Puts the ink on the panel over whatever of the picture's paper lies in
// the screen region x/y/w/h (the picture itself must already be there)
void overlayInk(int x, int y, int w, int h) {
    if (!inkReady_) return;
    // One BTE per band of rows with the same shift: above the bar none,
    // below it the slant (kSlant)
    auto band = [&](int y0, int y1, int shift) {           // picture rows y0..y1
        const int ix = printerX_ + kInkX - shift;
        const int sx0 = std::max(x, ix), sx1 = std::min(x + w, ix + kInkW);
        const int sy0 = std::max(y, printerY_ + y0), sy1 = std::min(y + h, printerY_ + y1);
        if (sx1 <= sx0 || sy1 <= sy0) return;
        display_->andLayerRegion(LT7683::kPrinterInkAddr, kInkW,
                                 static_cast<std::int16_t>(sx0 - ix),
                                 static_cast<std::int16_t>(sy0 - printerY_ - kPaperY0),
                                 /*dstAddr=*/0, SCREEN_MAX_X,
                                 static_cast<std::int16_t>(sx0), static_cast<std::int16_t>(sy0),
                                 static_cast<std::uint16_t>(sx1 - sx0), static_cast<std::uint16_t>(sy1 - sy0));
    };
    band(kPaperY0, kBarY0, 0);
    int start = kBarY1;
    for (int r = kBarY1 + 1; r <= kPaperY1; ++r) {
        if (r == kPaperY1 || kSlant[r - kBarY1] != kSlant[start - kBarY1]) {
            band(start, r, kSlant[start - kBarY1]);
            start = r;
        }
    }
}

void loadPaper() {
    inkReady_ = printerLoaded_;
    if (!inkReady_) return;
    displayMirror_addLayer(LT7683::kPrinterInkAddr, kInkW * 2UL * kInkBufH, 0, 1, 0);   // changes
    redrawInk();
}

// The paper as it is now, on the panel (if the Printer view is showing)
void showPaper() {
    if (output_ != DisplayOutput::Printer) return;
    const int maxSlant = kSlant[kPaperY1 - kBarY1 - 1];
    drawPrinterRegion(static_cast<std::int16_t>(printerX_ + kInkX - maxSlant),
                      static_cast<std::int16_t>(printerY_ + kPaperY0), kInkW + maxSlant, kInkH);
}

// The paper moved by hand to `off` (clamped), on the panel at once
void setPaperOff(int off) {
    off = std::clamp(off, -kMaxPullUp, maxPullDown());
    const int dy = off - paperOff_;
    if (dy == 0) return;
    paperOff_ = off;
    shiftInk(dy);
    showPaper();
}

// One step of the paper (from views_poll())
void pollPaper() {
    if (!inkReady_ || printer_ == nullptr) return;
    // PAPER ADVANCE held: the paper keeps moving
    if (printer_->advanceDown() && time_reached(advanceRepeat_)) {
        printer_->feedPaper();
        advanceRepeat_ = make_timeout_time_ms(kAdvanceRepeatMs);
    }
    const std::uint32_t total = printer_->lineCount();
    const bool showing = output_ == DisplayOutput::Printer;
    // Held by a finger: nothing else moves it meanwhile (new lines wait) --
    // but held still after a drag, it goes on in that direction
    if (paperDrag_) {
        if (!showing || dragDir_ == 0 || paperPhase_ != PaperPhase::Idle) return;
        if (!autoScroll_) {
            if (absolute_time_diff_us(holdSince_, get_absolute_time()) < kHoldMs * 1000ll) return;
            autoScroll_ = true;
            autoStart_ = get_absolute_time();
            autoNext_ = nil_time;
        }
        if (!time_reached(autoNext_)) return;
        autoNext_ = make_timeout_time_ms(kAutoStepMs);
        // A line a step; after 1.5 s two, after 3 s four, after 5 s eight
        // (8192 lines: about a minute)
        const std::int64_t heldMs = absolute_time_diff_us(autoStart_, get_absolute_time()) / 1000;
        const int lines = heldMs >= 5000 ? 8 : heldMs >= 3000 ? 4 : heldMs >= 1500 ? 2 : 1;
        setPaperOff(paperOff_ + dragDir_ * lines * kLinePx);   // (stops at either end: clamped)
        return;
    }
    // Tearing off: the paper slides away sideways, then it's empty
    if (tearDir_ != 0) {
        if (!time_reached(tearNext_)) return;
        tearNext_ = make_timeout_time_ms(kTearStepMs);
        const int step = (kInkW + kTearSteps - 1) / kTearSteps;
        if (++tearStep_ < kTearSteps && showing) {
            const int keep = kInkW - step;
            const std::int16_t from = static_cast<std::int16_t>(tearDir_ < 0 ? step : 0);
            const std::int16_t to = static_cast<std::int16_t>(tearDir_ < 0 ? 0 : step);
            display_->copyLayerRegion(LT7683::kPrinterInkAddr, kInkW, from, 0,
                                      LT7683::kPrinterInkStageAddr, kInkW, 0, 0,
                                      static_cast<std::uint16_t>(keep), kInkBufH);
            display_->copyLayerRegion(LT7683::kPrinterInkStageAddr, kInkW, 0, 0,
                                      LT7683::kPrinterInkAddr, kInkW, to, 0,
                                      static_cast<std::uint16_t>(keep), kInkBufH);
            {
                LayerDrawScope scope(LT7683::kPrinterInkAddr, kInkW);
                display_->setActiveWindow(0, 0, kInkW - 1, SCREEN_MAX_Y - 1);
                display_->fillRect(static_cast<std::int16_t>(tearDir_ < 0 ? keep : 0), 0,
                                   static_cast<std::int16_t>(step), kInkBufH, 0xFFFF);
            }
            display_->setActiveWindow(0, 0, SCREEN_MAX_X - 1, SCREEN_MAX_Y - 1);
            screen_->reassertRenderState();
            showPaper();
            return;
        }
        tearDir_ = 0;
        printer_->clearPaper();
        paperOff_ = 0;
        paperReturn_ = false;
        redrawInk();
        showPaper();
        return;
    }
    // Moved by hand, and something to print (or asked to): back to the
    // print head first, gliding (a quarter of a second or so)
    if (paperOff_ != 0 && paperPhase_ == PaperPhase::Idle &&
        (paperReturn_ || paperShown_ < total || printer_->advanceDown() || !showing)) {
        if (!showing) { paperOff_ = 0; paperReturn_ = false; redrawInk(); return; }
        if (!time_reached(returnNext_)) return;
        returnNext_ = make_timeout_time_ms(kReturnStepMs);
        const int step = std::max(8, std::abs(paperOff_) / 3);
        setPaperOff(paperOff_ > 0 ? std::max(0, paperOff_ - step) : std::min(0, paperOff_ + step));
        return;
    }
    if (paperOff_ == 0) paperReturn_ = false;
    if (paperPhase_ == PaperPhase::Idle) {
        if (paperDirty_) {
            paperDirty_ = false;
            redrawInk();
            showPaper();
            return;
        }
        if (paperShown_ >= total) return;
        const std::uint32_t behind = total - paperShown_;
        if (!showing || behind > 2) {
            // Catch up at once: line by line, or all of it if that's more
            // than the window holds
            if (behind > static_cast<std::uint32_t>(kInkH / kLinePx)) {
                redrawInk();
            } else {
                const int headWas = headX_;
                for (; paperShown_ < total; ++paperShown_) {
                    const CPrinter::Line* l = printer_->line(paperShown_);
                    if (!blankLine(l)) {
                        renderInkLine(kInkH, l);          // under the edge ...
                        headX_ = headX_ == kHeadMaxX ? kHeadMinX : kHeadMaxX;
                    }
                    feedInk(kLinePx);                     // ... and up into view
                }
                if (headX_ != headWas) showHeadArea(kHeadMinX, kHeadMaxX);
            }
            showPaper();
            return;
        }
        // A line to print: the head glides over first; a blank one only feeds
        const CPrinter::Line* l = printer_->line(paperShown_);
        if (blankLine(l)) {
            paperPhase_ = PaperPhase::Feed;
        } else {
            renderInkLine(kInkH, l);                      // printed, not in view yet
            paperPhase_ = PaperPhase::Glide;
            headFrom_ = headX_;
            headTo_ = headX_ == kHeadMaxX ? kHeadMinX : kHeadMaxX;
        }
        paperStep_ = 0;
        paperNext_ = nil_time;
    }
    if (!time_reached(paperNext_)) return;
    if (paperPhase_ == PaperPhase::Glide) {
        // The head to the other side, smoothly (slow at the ends)
        paperNext_ = make_timeout_time_ms(kGlideStepMs);
        ++paperStep_;
        const float t = static_cast<float>(paperStep_) / kGlideSteps;
        const float e = t * t * (3 - 2 * t);
        const int old = headX_;
        headX_ = headFrom_ + static_cast<int>(std::lround((headTo_ - headFrom_) * e));
        showHeadArea(std::min(old, headX_), std::max(old, headX_));
        if (paperStep_ >= kGlideSteps) {
            paperPhase_ = PaperPhase::Feed;
            paperStep_ = 0;
        }
        return;
    }
    // Feed: the paper up a line, the new line comes into view
    paperNext_ = make_timeout_time_ms(kFeedStepMs);
    feedInk(kLinePx / kFeedSteps);
    showPaper();
    if (++paperStep_ >= kFeedSteps) {
        paperPhase_ = PaperPhase::Idle;
        ++paperShown_;
    }
}

// Touch-sensitive areas, in hp82162a.bmp's own coordinates
struct PrtArea { PrinterHotspot id; std::int16_t x0, y0, x1, y1; };   // inclusive-exclusive
constexpr PrtArea kPrtAreas[] = {
    { PrinterHotspot::Power,     100, 430, 232, 476 },   // OFF - ON switch and labels
    { PrinterHotspot::Intensity, 240, 418, 412, 479 },   // INTENSITY, LIGHT / DARK
    { PrinterHotspot::Mode,      240, 479, 412, 548 },   // TRACE, MAN - MODE - NORM
    { PrinterHotspot::Print,     490, 436, 578, 512 },   // PRINT key and label
    { PrinterHotspot::Advance,   646, 438, 734, 524 },   // PAPER ADVANCE key and label
};

// Draws whichever view is now current on the main window: resumes Screen
// for Display (its own full() redraw), otherwise the plot or Tape image.
void revealCurrentView() {
    if (output_ == DisplayOutput::Display) {
        screen_->resume();       // un-suspends + its own full() redraw
    } else {
        views_redraw();    // plot or Tape image
    }
}

}  // namespace

// The yellow box used for the view-switch splash and for short messages
// (views_showMessage()): `text` centred at the given built-in font
// scale, shown for `ms` -- views_poll() removes it.
void views_impl::showSplashBox(const char* text, std::uint8_t scale, std::uint32_t ms) {
    display_->setActiveWindow(0, 0, SCREEN_MAX_X - 1, SCREEN_MAX_Y - 1);
    // The box takes PIP-1 (and hides it afterwards): an open info box or
    // device list there would be left "open" but unseen, eating every touch
    boardui_closePipBoxes();
    display_->beginOverlayDraw();        // draw into the PIP layer

    // LT7683 hardware font: 8px base glyph width, scaling to
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
    // already includes views.h for DisplayOutput).
    display_->fillRect(splashX, splashY, splashW, splashH, 0xEDC0);
    display_->rect(splashX, splashY, splashW, splashH, 0xFFFF);

    display_->txtColor(0x0000, 0xEDC0);  // black text on the yellow box
    display_->txtSize(scale);
    // Always use the chip's built-in CGROM font here, never the custom
    // hp82163_font.hpp glyphs -- same rule as every menu dialog ...
    display_->selectBuiltinFont();
    display_->txtSetCursor(splashX + (splashW - textW) / 2, splashY + (splashH - charH) / 2);
    display_->txtWrite(text);

    display_->endOverlayDraw();          // canvas back to the live panel
    screen_->reassertRenderState();      // undo the splash's text size/colour
    display_->showPipOverlay(splashX, splashY, splashW, splashH);
    splashVisible_ = true;
    splashHideDeadline_ = make_timeout_time_ms(ms);
    // The text above moved the chip's one text cursor into the box: back
    // to where the display has it. (With a menu open the cursor is hidden,
    // and closing the menu puts it back.)
    if (!boardui_isMenuOpen()) views_refreshCursor();
}

void views_init(DisplayDriver* display, Screen* screen, CDevice* plotterDevice, HpglInterpreter* hpgl) {
    display_ = display;
    screen_ = screen;
    plotInit(plotterDevice, hpgl);
    mediamap_init(display);
    loadTapeLayer();
    if (tapeLoaded_) {
        loadCassette();
        loadLidOpen();
        loadLeds();
        loadReels();
    }
    loadPrinterLayer();
    if (printerLoaded_) {
        loadPrinterParts();
        loadPaper();
    }
}

void views_setTapeFile(const std::string& filename) {
    tapeFile_ = filename;
    composeCassetteLabel();
    refreshCassette();
}

void views_showMessage(const char* text) {
    showSplashBox(text, 1, 2500);
}

void views_setDrive(CDrive* drive) {
    drive_ = drive;
}

CDrive* views_drive() {
    return drive_;
}

void views_setPrinter(CPrinter* printer) { printer_ = printer; }
CPrinter* views_printer() { return printer_; }

void views_printerPrint(bool down) {
    if (printer_ == nullptr) return;
    if (down) prtPrintUntil_ = make_timeout_time_ms(kPrtKeyShowMs);
    printer_->pressPrint(down);
    pollPrinterPanel();                  // show it at once
}

void views_printerAdvance(bool down) {
    if (printer_ == nullptr) return;
    if (down) {
        prtAdvUntil_ = make_timeout_time_ms(kPrtKeyShowMs);
        advanceRepeat_ = make_timeout_time_ms(500);      // held: keeps feeding after this
    }
    printer_->pressAdvance(down);
    pollPrinterPanel();
}

void views_setDemoTape(bool busy) { demoTape_ = busy; }

void views_clearPaper() {
    if (printer_ == nullptr) return;
    printer_->clearPaper();
    paperOff_ = 0;
    paperReturn_ = false;
    if (printerLoaded_ && inkReady_) {
        redrawInk();
        if (output_ == DisplayOutput::Printer) drawPrinterRegion(0, 0, SCREEN_MAX_X, SCREEN_MAX_Y);
    }
}

bool views_paperDragBegin(int x, int y) {
    if (output_ != DisplayOutput::Printer || !inkReady_ || printer_ == nullptr || views_isSplashVisible())
        return false;
    // The paper's window, with a little room on the sides
    const int x0 = printerX_ + kPaperX0 - 24, x1 = printerX_ + kPaperX0 + kPaperW + 24;
    const int y0 = printerY_ + kPaperY0, y1 = printerY_ + kPaperY1 + 8;
    if (x < x0 || x >= x1 || y < y0 || y >= y1) return false;
    if (tearDir_ != 0) return true;          // (being torn off: the touch does nothing)
    paperDrag_ = true;
    paperReturn_ = false;
    dragY0_ = y;
    dragOff0_ = paperOff_;
    holdY_ = y;
    holdSince_ = get_absolute_time();
    dragDir_ = 0;
    autoScroll_ = false;
    atEdge_ = false;
    tearGesture_ = 0;
    return true;
}
void views_paperDragMove(int x, int y) {
    if (!paperDrag_) return;
    // Moved (more than a finger's tremble): its direction, and the hold
    // starts again from here; going on by itself ends -- the finger has it
    if (std::abs(y - holdY_) > kHoldSlop) {
        dragDir_ = y > holdY_ ? 1 : -1;
        // (going on ends: the finger takes the paper from where it was held)
        if (autoScroll_) { autoScroll_ = false; dragY0_ = holdY_; dragOff0_ = paperOff_; }
        holdY_ = y;
        holdSince_ = get_absolute_time();
    }
    if (autoScroll_) return;                  // (held still: the paper goes on by itself)
    // While a line is still on its way (the head, the feed) it finishes first
    if (paperPhase_ != PaperPhase::Idle) { dragY0_ = y; dragOff0_ = paperOff_; return; }
    setPaperOff(dragOff0_ + (y - dragY0_));
    // At the tear-off edge, then sideways: tear it off (asked at the end)
    if (paperOff_ == -kMaxPullUp && printer_->keptLines() > 0) {
        if (!atEdge_) { atEdge_ = true; edgeX_ = x; edgeY_ = y; }
        if (y < edgeY_) edgeY_ = y;           // (the finger going on up: the paper stays at the edge)
        if (std::abs(x - edgeX_) >= kTearDx && std::abs(y - edgeY_) < kTearMaxDy) tearGesture_ = x > edgeX_ ? 1 : -1;
    } else {
        atEdge_ = false;
    }
}
void views_paperDragEnd() { paperDrag_ = false; autoScroll_ = false; }
int views_paperOffset() { return paperOff_; }
bool views_takeTearGesture(int& dir) {
    if (tearGesture_ == 0) return false;
    dir = tearGesture_;
    tearGesture_ = 0;
    return true;
}
void views_tearOff(int dir) {
    if (printer_ == nullptr) return;
    if (output_ != DisplayOutput::Printer || !inkReady_) { views_clearPaper(); return; }
    tearDir_ = dir < 0 ? -1 : 1;
    tearStep_ = 0;
    tearNext_ = nil_time;
}
int views_paperLines() { return printer_ != nullptr ? static_cast<int>(printer_->keptLines()) : 0; }
void views_reloadPaper() {
    // The printer's lines were replaced (a saved printout loaded): drawn
    // afresh, at the live position
    paperOff_ = 0;
    paperReturn_ = false;
    tearDir_ = 0;
    if (printerLoaded_ && inkReady_) {
        redrawInk();
        if (output_ == DisplayOutput::Printer) drawPrinterRegion(0, 0, SCREEN_MAX_X, SCREEN_MAX_Y);
    }
}
void views_paperScrollBy(int lines) {
    if (output_ != DisplayOutput::Printer || !inkReady_ || paperPhase_ != PaperPhase::Idle) return;
    paperReturn_ = false;
    setPaperOff(paperOff_ + lines * kLinePx);
}
int views_paperWindowLines() { return kInkH / kLinePx; }
bool views_paperScrolled() { return paperOff_ != 0; }
void views_paperToLive() { if (paperOff_ != 0) paperReturn_ = true; }

void views_setTapeEjected(bool ejected) {
    if (ejected == tapeEjected_) return;
    tapeEjected_ = ejected;
    refreshCassette();
}

bool views_isAvailable(DisplayOutput mode) {
    if (mode == DisplayOutput::Tape) return tapeLoaded_;
    if (mode == DisplayOutput::Printer) return printerLoaded_;
    if (mode == DisplayOutput::Video80) return video80_ != nullptr;
    return true;
}

void views_setVideo80(Video80* v) {
    video80_ = v;
    if (v == nullptr) return;
    // It draws only while its view is shown
    v->setDrawGate([] {
        return output_ == DisplayOutput::Video80;
    });
    // No blinking cursor while the menu is open: the menu's text moves the
    // chip's one text cursor, so it would jump into (or beside) the menu.
    // Closing the menu puts it back (views_refreshCursor()).
    v->setCursorGate([] { return !boardui_isMenuOpen(); });
}

Video80* views_video80() { return video80_; }

void views_refreshCursor() {
    if (screen_ != nullptr) screen_->refreshCursor();      // (nothing while suspended)
    if (output_ == DisplayOutput::Video80 && video80_ != nullptr && video80_->active()) video80_->catchUp();
}

void views_showView(DisplayOutput mode, CDevice* dev) {
    if (mode == DisplayOutput::HwTest) hwtest_enter();
    if (mode == DisplayOutput::MediaMap) mediamap_enter();
    switchView(mode, dev, false);
}

void views_setDisplayModel(bool v80) {
    if (v80 == displayV80_) return;
    displayV80_ = v80;
    // Showing the display: the other one takes over (no name box)
    if (output_ != DisplayOutput::Display && output_ != DisplayOutput::Video80) return;
    const DisplayOutput to = displayOutput();
    if (to == output_) return;
    switchView(to, viewDevice_, false);
}

void views_redraw() {
    if (output_ == DisplayOutput::Video80) {
        if (video80_ != nullptr) {
            if (video80_->active()) video80_->drawAll();
            else video80_->setActive(true);      // (draws it all)
        }
        return;
    }
    if (output_ == DisplayOutput::HwTest) {
        hwtest_drawAll();
        return;
    }
    if (output_ == DisplayOutput::MediaMap) {
        mediamap_drawAll();
        return;
    }
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
    if (output_ == DisplayOutput::Printer) {
        drawPrinterRegion(0, 0, SCREEN_MAX_X, SCREEN_MAX_Y);
        return;
    }
    if (output_ == DisplayOutput::Plotter) plotRedraw();
}

void views_redrawRegion(std::int16_t x0, std::int16_t y0,
                        std::int16_t w, std::int16_t h) {
    if (output_ == DisplayOutput::Tape) {
        drawTapeRegion(x0, y0, w, h);
        return;
    }
    if (output_ == DisplayOutput::Printer) {
        drawPrinterRegion(x0, y0, w, h);
        return;
    }
    if (output_ == DisplayOutput::Video80) {
        if (video80_ != nullptr) video80_->drawRegion(x0, y0, w, h);
        return;
    }
    if (output_ == DisplayOutput::Plotter) plotRedrawRegion(x0, y0, w, h);
}

// Switches the panel to `mode`, for device `dev` (its title goes in the
// splash). Tape -> Tape with another drive also counts as a switch.
// splash = false: switch at once, no name box (the screen saver)
// A device's view opened from the loop map's details box: X goes back there
static bool fromLoopMap_ = false;

void views_impl::switchView(DisplayOutput mode, CDevice* dev, bool splash) {
    if (!views_isAvailable(mode)) return;
    if (mode == output_ && dev == viewDevice_) return;
    fromLoopMap_ = false;                    // (another view: see views_showDeviceFromLoopMap())
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
        composeCassetteLabel();
    }
    // Leaving the Hardware test page: it gives back the LEDs and pixels
    if (output_ == DisplayOutput::HwTest && mode != DisplayOutput::HwTest) hwtest_leave();
    // Leaving the 80-column view: it stops drawing (and hides its cursor)
    if (output_ == DisplayOutput::Video80 && mode != DisplayOutput::Video80 && video80_ != nullptr)
        video80_->setActive(false);
    if (mode == DisplayOutput::Printer && printer_ != nullptr) {
        // Draw the front panel as it is now (no change-by-change redraws)
        prtPowerShown_ = printer_->enabled();
        prtModeShown_ = static_cast<int>(printer_->mode());
        prtIntensityShown_ = printer_->intensity();
        prtPrintShown_ = prtAdvShown_ = false;
    }
    output_ = mode;
    if (mode != DisplayOutput::Display) {
        // Stop Screen from drawing text over the plot, and turn off the
        // hardware cursor explicitly -- suspend() alone doesn't touch
        // whatever cursor state the chip already had active, which
        // would otherwise just keep blinking autonomously via the chip's
        // own hardware timer regardless of software suspension.
        screen_->suspend();
        screen_->hideCursorHardware();
    }
    if (!splash) {
        revealCurrentView();
        return;
    }
    std::string title = dev ? views_viewTitle(dev) : std::string(outputName(mode));
    for (char& c : title) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    showSplashBox(title.c_str(), 2, kSplashMs);
    // Draw the new view now, hidden behind the full-screen splash overlay
    // -- it's already complete when views_poll() removes the overlay.
    revealCurrentView();
}

// The view HIPI starts with, and goes to when the one it would go back to
// has gone: the first device in loop order that is switched on and has a
// view -- else the loop map. (Not the printer while it's switched off,
// though its view could be shown.)
static void showFirstView(bool splash) {
    for (CDevice* d : devices) {
        if (d != nullptr && d->enabled() && deviceHasView(d)) {
            switchView(outputFor(d->viewKind()), d, splash);
            return;
        }
    }
    switchView(DisplayOutput::LoopMap, nullptr, splash);
}

void views_showStartView() { showFirstView(false); }

void views_showDevice(CDevice* dev) {
    if (deviceHasView(dev)) switchView(outputFor(dev->viewKind()), dev);
}

CDevice* views_viewDevice() { return viewDevice_; }

// ── View chosen from the HP-41 ("ESC # V <addr>") ───────────────────────
// -1 = no request. Written from HP-IL frame handling, read from the main
// loop -- same core, so a plain volatile is enough; a newer request
// simply replaces one not yet taken.
static volatile int viewRequest_ = -1;

void views_requestViewByAddress(std::uint8_t addr) { viewRequest_ = addr; }

bool views_takeViewRequest(std::uint8_t& addr) {
    const int r = viewRequest_;
    if (r < 0) return false;
    viewRequest_ = -1;
    addr = static_cast<std::uint8_t>(r);
    return true;
}

bool views_showAddress(std::uint8_t addr) {
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
        LOGF("\r\n * ESC # V %u: showing %s", addr, views_viewTitle(dev).c_str());
        switchView(outputFor(dev->viewKind()), dev);
        return true;
    }
    LOGF("\r\n * ESC # V %u: no HIPI device at that address -- ignored", addr);
    return false;
}

// The view to go back to when leaving the Analyzer
static CDevice* beforeAnalyzer_ = nullptr;
static DisplayOutput beforeAnalyzerOutput_ = DisplayOutput::Display;

void views_showAnalyzer() {
    if (output_ == DisplayOutput::Analyzer) return;
    beforeAnalyzer_ = viewDevice_;
    beforeAnalyzerOutput_ = output_;
    switchView(DisplayOutput::Analyzer, nullptr);
}

// The view to go back to when leaving the Hardware test page
static CDevice* beforeHwTest_ = nullptr;
static DisplayOutput beforeHwTestOutput_ = DisplayOutput::Display;

void views_showHwTest() {
    if (output_ == DisplayOutput::HwTest) return;
    beforeHwTest_ = viewDevice_;
    beforeHwTestOutput_ = output_;
    hwtest_enter();
    switchView(DisplayOutput::HwTest, nullptr);
}

void views_leaveHwTest() {
    if (output_ != DisplayOutput::HwTest) return;
    const DisplayOutput o = beforeHwTestOutput_;
    if (o == DisplayOutput::Analyzer || o == DisplayOutput::LoopMap || o == DisplayOutput::Signals) {
        switchView(o, nullptr);
    } else if (deviceHasView(beforeHwTest_)) {
        switchView(outputFor(beforeHwTest_->viewKind()), beforeHwTest_);
    } else {
        showFirstView(true);
    }
}

// The view to go back to when leaving the media map
static CDevice* beforeMediaMap_ = nullptr;
static DisplayOutput beforeMediaMapOutput_ = DisplayOutput::Display;

void views_showMediaMap() {
    if (output_ == DisplayOutput::MediaMap) return;
    beforeMediaMap_ = viewDevice_;
    beforeMediaMapOutput_ = output_;
    mediamap_enter();
    switchView(DisplayOutput::MediaMap, nullptr);
}

void views_leaveMediaMap() {
    if (output_ != DisplayOutput::MediaMap) return;
    const DisplayOutput o = beforeMediaMapOutput_;
    if (o == DisplayOutput::Analyzer || o == DisplayOutput::LoopMap || o == DisplayOutput::Signals) {
        switchView(o, nullptr);
    } else if (deviceHasView(beforeMediaMap_)) {
        switchView(outputFor(beforeMediaMap_->viewKind()), beforeMediaMap_);
    } else {
        showFirstView(true);
    }
}

// The view to go back to when leaving the loop map
static CDevice* beforeLoopMap_ = nullptr;
static DisplayOutput beforeLoopMapOutput_ = DisplayOutput::Display;

void views_showLoopMap() {
    if (output_ == DisplayOutput::LoopMap) return;
    beforeLoopMap_ = viewDevice_;
    beforeLoopMapOutput_ = output_;
    switchView(DisplayOutput::LoopMap, nullptr);
}

const char* views_viewName(CDevice* dev) {
    return deviceHasView(dev) ? outputName(outputFor(dev->viewKind())) : nullptr;
}

void views_showDeviceFromLoopMap(CDevice* dev) {
    if (!deviceHasView(dev)) return;
    switchView(outputFor(dev->viewKind()), dev, /*splash=*/false);   // (chosen by name: no name box)
    fromLoopMap_ = output_ != DisplayOutput::LoopMap;
}

bool views_cameFromLoopMap() { return fromLoopMap_ && output_ != DisplayOutput::LoopMap; }

bool views_backToLoopMap() {
    if (!views_cameFromLoopMap()) return false;
    // (straight back: the loop map's own "view before" stays what it was)
    switchView(DisplayOutput::LoopMap, nullptr, false);
    return true;
}

void views_leaveLoopMap() {
    if (output_ != DisplayOutput::LoopMap) return;
    if (beforeLoopMapOutput_ == DisplayOutput::Analyzer) {
        switchView(DisplayOutput::Analyzer, nullptr);
    } else if (deviceHasView(beforeLoopMap_)) {
        switchView(outputFor(beforeLoopMap_->viewKind()), beforeLoopMap_);
    } else {
        showFirstView(true);
    }
}

// The view to go back to when leaving the HP-IL signals view
static CDevice* beforeSignals_ = nullptr;
static DisplayOutput beforeSignalsOutput_ = DisplayOutput::Display;

void views_showSignals() {
    if (output_ == DisplayOutput::Signals) return;
    beforeSignals_ = viewDevice_;
    beforeSignalsOutput_ = output_;
    switchView(DisplayOutput::Signals, nullptr);
}

void views_leaveSignals() {
    if (output_ != DisplayOutput::Signals) return;
    if (beforeSignalsOutput_ == DisplayOutput::Analyzer || beforeSignalsOutput_ == DisplayOutput::LoopMap) {
        switchView(beforeSignalsOutput_, nullptr);
    } else if (deviceHasView(beforeSignals_)) {
        switchView(outputFor(beforeSignals_->viewKind()), beforeSignals_);
    } else {
        showFirstView(true);
    }
}

// The view to go back to when the character table closes
static CDevice* beforeCharTable_ = nullptr;
static DisplayOutput beforeCharTableOutput_ = DisplayOutput::Display;

void views_showCharTable() {
    if (output_ == DisplayOutput::CharTable) return;
    beforeCharTable_ = viewDevice_;
    beforeCharTableOutput_ = output_;
    chartable_reset();                       // (the table, no character's box)
    switchView(DisplayOutput::CharTable, nullptr, /*splash=*/false);
}

void views_leaveCharTable() {
    if (output_ != DisplayOutput::CharTable) return;
    const DisplayOutput o = beforeCharTableOutput_;
    if (o == DisplayOutput::Analyzer || o == DisplayOutput::LoopMap || o == DisplayOutput::Signals) {
        switchView(o, nullptr, false);
    } else if (deviceHasView(beforeCharTable_)) {
        switchView(outputFor(beforeCharTable_->viewKind()), beforeCharTable_, false);
    } else {
        showFirstView(false);
    }
}

// The view to go back to when the clock screen saver ends
static CDevice* beforeClock_ = nullptr;
static DisplayOutput beforeClockOutput_ = DisplayOutput::Display;

void views_showClock() {
    if (output_ == DisplayOutput::Clock) return;
    beforeClock_ = viewDevice_;
    beforeClockOutput_ = output_;
    switchView(DisplayOutput::Clock, nullptr, /*splash=*/false);
}

void views_leaveClock() {
    if (output_ != DisplayOutput::Clock) return;
    if (beforeClockOutput_ == DisplayOutput::Analyzer) {
        switchView(DisplayOutput::Analyzer, nullptr, false);
    } else if (deviceHasView(beforeClock_)) {
        switchView(outputFor(beforeClock_->viewKind()), beforeClock_, false);
    } else {
        showFirstView(false);
    }
}

void views_leaveAnalyzer() {
    if (output_ != DisplayOutput::Analyzer) return;
    if (deviceHasView(beforeAnalyzer_)) {
        switchView(outputFor(beforeAnalyzer_->viewKind()), beforeAnalyzer_);
    } else {
        const DisplayOutput o = beforeAnalyzerOutput_;
        const bool deviceView = o == DisplayOutput::Display || o == DisplayOutput::Video80 ||
                                o == DisplayOutput::Plotter || o == DisplayOutput::Tape ||
                                o == DisplayOutput::Printer;
        // (its device has gone: the first one that has a view, else the loop map)
        if (deviceView) showFirstView(true);
        else switchView(o, nullptr);
    }
}

bool views_isPage(DisplayOutput o) {
    return o == DisplayOutput::HwTest || o == DisplayOutput::LoopMap || o == DisplayOutput::MediaMap ||
           o == DisplayOutput::CharTable || o == DisplayOutput::Analyzer || o == DisplayOutput::Signals;
}

void views_leavePage() {
    switch (output_) {
        case DisplayOutput::HwTest:    views_leaveHwTest(); break;
        case DisplayOutput::LoopMap:   views_leaveLoopMap(); break;
        case DisplayOutput::MediaMap:  views_leaveMediaMap(); break;
        case DisplayOutput::CharTable: views_leaveCharTable(); break;
        case DisplayOutput::Analyzer:  views_leaveAnalyzer(); break;
        case DisplayOutput::Signals:   views_leaveSignals(); break;
        default: break;
    }
}

bool views_shortcutTouch(int x, int y) {
    const int y0 = (SCREEN_MAX_Y - kShortcutH) / 2;
    return x >= 0 && x < kShortcutW && y >= y0 && y < y0 + kShortcutH && !views_isPage(output_);
}

bool views_openShortcut(ShortcutView v) {
    // Showing it already: the same touch goes back
    switch (v) {
        case ShortcutView::LoopMap:
            if (output_ == DisplayOutput::LoopMap) views_leaveLoopMap(); else views_showLoopMap();
            return true;
        case ShortcutView::Analyzer:
            if (output_ == DisplayOutput::Analyzer) views_leaveAnalyzer(); else views_showAnalyzer();
            return true;
        case ShortcutView::Signals:
            if (output_ == DisplayOutput::Signals) views_leaveSignals(); else views_showSignals();
            return true;
        case ShortcutView::CharTable:
            if (output_ == DisplayOutput::CharTable) views_leaveCharTable(); else views_showCharTable();
            return true;
        case ShortcutView::HwTest:     // (left with its own X: the zone is off there)
            views_showHwTest();
            return true;
        case ShortcutView::Off:
            break;
    }
    return false;
}

const char* views_shortcutName(ShortcutView v) {
    switch (v) {
        case ShortcutView::LoopMap:   return "Loop map";
        case ShortcutView::Analyzer:  return "Analyzer";
        case ShortcutView::Signals:   return "HP-IL signals";
        case ShortcutView::CharTable: return "Characters";
        case ShortcutView::HwTest:    return "Hardware test";
        case ShortcutView::Off:       return "Off";
    }
    return "?";
}

std::vector<CDevice*> views_viewDevices() {
    std::vector<CDevice*> list;
    for (CDevice* dev : devices) {
        if (deviceHasView(dev)) list.push_back(dev);
    }
    return list;
}

std::string views_viewTitle(CDevice* dev) {
    std::string title;
    switch (dev->viewKind()) {
        case ViewKind::Display: title = "Display"; break;
        case ViewKind::Plotter: title = "Plotter"; break;
        case ViewKind::Tape:    title = "Tape";    break;
        case ViewKind::Printer: title = "Printer"; break;
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

bool views_isSplashVisible() { return splashVisible_; }

void views_poll() {
    plotPoll();             // Plotter view: digitizing, the ERROR light
    if (output_ == DisplayOutput::MediaMap && !views_isSplashVisible())
        mediamap_poll(to_ms_since_boot(get_absolute_time()));    // the head, a changed directory
    pollPrinterPanel();     // Printer view: switches, POWER light, keys
    pollPaper();            // ... and the paper
    // Status LEDs -- redraw only on change (a single BTE copy each)
    if (ledsLoaded_ && drive_ != nullptr) {
        const bool power = demoTape_ || drive_->poweredUp();        // POWER light
        const bool busy  = demoTape_ || (power && drive_->busyLight());   // BUSY light, per the manual
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
    if (reelsLoaded_ && drive_ != nullptr && (drive_->isBusy() || demoTape_) &&
        output_ == DisplayOutput::Tape && cassetteShown() &&
        time_reached(reelNext_)) {
        // Rewind (REWIND button): faster, and the reels turn the other way
        const bool rewinding = !demoTape_ && drive_->isRewinding();
        reelNext_ = make_timeout_time_ms(rewinding ? kReelRewindFrameMs : kReelFrameMs);
        reelFrame_ = (reelFrame_ + (rewinding ? kReelFrames - 1 : 1)) % kReelFrames;
        for (int i = 0; i < 2; ++i) drawReel(i);
    }
    if (splashVisible_ && time_reached(splashHideDeadline_)) {
        splashVisible_ = false;
        display_->hidePipOverlay();      // view underneath is already drawn
    }
}

DisplayOutput views_output() { return output_; }

void views_cycleOutput(bool forward) {
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
    // No device has a view at all: the loop map
    showFirstView(true);
}

bool views_isActive() { return output_ != DisplayOutput::Display; }

TapeHotspot views_tapeHitTest(std::uint16_t x, std::uint16_t y) {
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

PrinterHit views_printerHitTest(std::uint16_t x, std::uint16_t y) {
    PrinterHit hit;
    if (output_ != DisplayOutput::Printer || !printerLoaded_ || splashVisible_ || printer_ == nullptr)
        return hit;
    const int ix = static_cast<int>(x) - printerX_;      // screen -> picture coordinates
    const int iy = static_cast<int>(y) - printerY_;
    for (const PrtArea& a : kPrtAreas) {
        if (ix < a.x0 || ix >= a.x1 || iy < a.y0 || iy >= a.y1) continue;
        hit.spot = a.id;
        if (a.id == PrinterHotspot::Intensity) {
            // LIGHT / DARK: one step that way; on the switch: that position
            const int cur = printer_->intensity();
            if (ix < 291)      hit.value = std::max(0, cur - 1);
            else if (ix > 371) hit.value = std::min(CPrinter::kIntensities - 1, cur + 1);
            else hit.value = std::clamp(static_cast<int>(std::lround(static_cast<double>(ix - kPrtKnob0X) /
                                                                     kPrtIntensityStep)),
                                        0, CPrinter::kIntensities - 1);
        } else if (a.id == PrinterHotspot::Mode) {
            // MAN / NORM labels, TRACE (the label above), or the position touched
            if (ix < 291)                     hit.value = 0;
            else if (ix > 371)                hit.value = 2;
            else if (iy < 496)                hit.value = 1;
            else hit.value = std::clamp(static_cast<int>(std::lround(static_cast<double>(ix - kPrtKnob0X) /
                                                                     kPrtModeStep)), 0, 2);
        }
        return hit;
    }
    return hit;
}

}  // namespace hipi
