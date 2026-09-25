// SPDX-License-Identifier: MIT
//
// boardui.cpp -- see boardui.h for the module overview.

// usb_serial.h must come first -- it defines the LOGF macro that
// uidialog.hpp (pulled in via boardui.h), bmp_loader.hpp, and config.hpp
// all use in inline/header function bodies. Including it later left LOGF
// undefined at the point those headers were parsed.
#define MODULE "BOARDUI"

#include "usb_serial.h"
#include "plotterview.h"

#include "boardui.h"

#include <cstdio>
#include <cstring>
#include <vector>

#include "bmp_loader.hpp"
#include "config.hpp"
#include "hipi.h"      // DeviceInfo, hipi_enumerateDevices() -- "Devices" dialog
#include "hpil.h"      // hpilDevices
#include "drive.h"     // CDrive -- Tape view power switch
#include "pilbox.h"    // pilbox, CPilBox::isConnected()
#include "ui_buttons.hpp"
#include "pico/time.h"
#include "pico/stdlib.h"  // sleep_ms(), for the button strip's slide animation
#include "tusb.h"       // tud_mounted() -- keeps usb_connected live, see boardui_poll()
#include <algorithm>    // std::max, for the "Devices" dialog's scroll clamping

extern hipi::Config config;
extern std::uint8_t hpilDevices;

namespace hipi {

namespace {

DisplayDriver* display_ = nullptr;
Screen* screen_  = nullptr;
UiDialog* dialog_ = nullptr;
const char* version_ = "";

// Reads a BMP's pixel data right-aligned at (screen_width - width, y0),
// caching the pixels/dimensions in the out* parameters so a sub-region can
// be redrawn later without re-reading the file (see redrawButtonRegion()
// in ui_buttons.hpp).
bool drawBmpRightAligned(DisplayDriver* display, const char* path,
                         std::uint16_t screen_width, std::uint16_t y0,
                         std::vector<std::uint16_t>* outPixels,
                         std::uint16_t* outWidth,
                         std::uint16_t* outHeight,
                         std::uint16_t* outScreenX0,
                         bool alsoDraw = true) {
    std::uint16_t width = 0, height = 0;
    if (!peekBmpDimensions(path, width, height)) {
        LOGF("\r\n\t * Could not read BMP dimensions for <%s>!", path);
        return false;
    }
    const std::uint16_t x0 = static_cast<std::uint16_t>(screen_width - width);  // right edge
    if (outScreenX0) *outScreenX0 = x0;
    return drawBmpAt(display, path, static_cast<std::int16_t>(x0),
                     static_cast<std::int16_t>(y0),
                     outPixels, outWidth, outHeight, alsoDraw);
}

// ── Auto-hiding button strip ────────────────────────────────────────────────
//
// Normally hidden, so the text area gets the full panel width. Any touch
// anywhere on the screen -- whether that's a tap squarely on a button or
// just the start of a drag across the display -- reveals the strip and
// resets the countdown; hitTestButton() works purely on fixed screen
// coordinates, so it still recognizes touches in the (currently blank)
// button area even while hidden. The touch that reveals the strip is
// consumed by the reveal itself, not treated as an actual button press
// (the user couldn't see what they were pressing yet). It auto-hides again
// after kButtonStripHideMs of inactivity, but only while the menu is closed
// -- you need the buttons visible to navigate it.

// Cached copy of the button-strip bitmap, so a single button's
// sub-rectangle can be redrawn (e.g. shifted for "pressed" feedback)
// without re-reading the BMP from the SD card. Filled in by
// boardui_loadButtonStrip() at boot. RGB565 (2 bytes/pixel), matching the
// display's 16bpp mode.
std::vector<std::uint16_t> buttonStripPixels;
std::uint16_t buttonStripWidth = 0, buttonStripHeight = 0, buttonStripScreenX0 = 0;

bool buttonStripVisible = false;
absolute_time_t buttonStripHideDeadline;
constexpr std::uint32_t kButtonStripHideMs = 5000;
// Used only right when the menu explicitly closes (see
// boardui_onMenuClosed() below) -- much shorter than the normal
// inactivity timeout, since at that point the user has already shown
// they're done (picked "<--"/Exit), rather than just having gone quiet
// for a while mid-navigation.
constexpr std::uint32_t kButtonStripQuickHideMs = 500;

// Last commanded state of each status LED, so re-showing the strip (which
// redraws the whole cached bitmap, including their baked-in "off" look)
// can immediately reapply whichever state was actually current.
bool usbLedOn = false;
bool pilLedOn = false;

// Periodic live poll of USB/PILBOX connection state (see updateStatusLed()
// below and its call site in boardui_poll()).
absolute_time_t statusCheckDeadline = get_absolute_time();
constexpr std::uint32_t kStatusCheckMs = 250;

// Button-press visual feedback: redraw the button's bitmap shifted by
// (kPressDx, kPressDy) while held. kPressMargin (> the largest shift
// magnitude) is used for the baseline/restore redraws so any overflow
// outside the button's own rect -- in any direction -- gets cleaned up,
// not just the tight rect itself.
constexpr std::int16_t kPressDx = 5, kPressDy = -5;
constexpr std::int16_t kPressMargin = 8;
Button pressedButton = Button::None;

// Slide animation timing for showButtonStrip()/hideButtonStrip() below --
// short enough to feel snappy, several steps so the motion actually reads
// as a slide rather than a jump.
constexpr int kSlideSteps = 8;
constexpr std::uint32_t kSlideStepDelayMs = 30;
// Hiding felt rushed at the same pace as showing -- a bit longer per
// step here specifically, so it reads as a deliberate slide rather than
// a blink.
constexpr std::uint32_t kHideStepDelayMs = 40;

// Splits buttonStripWidth into kSlideSteps horizontal slices (the last
// one absorbs whatever doesn't divide evenly) -- used by
// showButtonStrip()/hideButtonStrip()'s animation below.
[[maybe_unused]] void computeSegmentWidths(std::uint16_t* out) {  // 5" only
    const auto base = static_cast<std::uint16_t>(buttonStripWidth / kSlideSteps);
    std::uint16_t assigned = 0;
    for (int i = 0; i < kSlideSteps - 1; ++i) {
        out[i] = base;
        assigned = static_cast<std::uint16_t>(assigned + base);
    }
    out[kSlideSteps - 1] = static_cast<std::uint16_t>(buttonStripWidth - assigned);
}

#ifdef DISPLAY_7INCH
// ── 7": button strip as a PIP-2 overlay ────────────────────────────────────
// The strip lives in its own small SDRAM layer (LT7683::kStripLayerAddr,
// stride = strip width rounded up to 4 pixels) and is shown with the
// chip's second PIP window. Sliding in/out only changes that window's
// width and position, so whatever is on the panel underneath -- text,
// plot or Tape image -- is never overwritten and needs no restoring.
// (The menu, info box, device list and view-switch splash use PIP-1,
// which is drawn on top of PIP-2 and never overlaps the strip.)
std::uint16_t stripStride = 0;   // strip layer's own stride (multiple of 4)
std::uint16_t stripPad = 0;      // columns left of the strip in its layer

// PIP window width for slide step i (1..kSlideSteps) -- PIP width and
// x position must be multiples of 4 pixels.
std::uint16_t stripVisibleWidth(int i) {
    if (i >= kSlideSteps) return stripStride;
    const auto w = static_cast<std::uint16_t>(((stripStride * i) / kSlideSteps + 3) & ~3);
    return std::max<std::uint16_t>(4, std::min(w, stripStride));
}

// Shows the leftmost visibleW columns of the strip layer at the panel's
// right edge (visibleW == 0 hides it).
void showStripPip(std::uint16_t visibleW) {
    if (visibleW == 0) {
        display_->hidePipWindow(2);
        return;
    }
    display_->showPipWindow(2, LT7683::kStripLayerAddr, stripStride,
                            /*srcX=*/0, /*srcY=*/0,
                            static_cast<std::int16_t>(SCREEN_MAX_X - visibleW), /*dstY=*/0,
                            visibleW, buttonStripHeight);
}
#endif

// Runs fn(stripX0) to draw part of the button strip (a pressed/released
// button, a status LED). stripX0 is where the strip's own column 0 is in
// the drawing target:
//   7": the strip's PIP layer (x = stripPad) -- the PIP overlay shows the
//       change immediately, whether the strip is visible or not.
//   5": the panel itself (x = buttonStripScreenX0), with the active window
//       temporarily widened to include the strip area. It must be narrowed
//       again afterwards: left at "full panel", a later Screen::full()
//       (e.g. on menu close) would clear the whole display instead of just
//       its own text area (confirmed on real hardware).
template <typename F>
void drawOnStrip(F&& fn) {
#ifdef DISPLAY_7INCH
    display_->beginLayerDraw(LT7683::kStripLayerAddr, stripStride);
    display_->setActiveWindow(0, 0, stripStride - 1, SCREEN_MAX_Y - 1);
    fn(stripPad);
    display_->endOverlayDraw();              // canvas back to the live panel
    if (screen_ != nullptr) screen_->reassertRenderState();
    display_->setActiveWindow(0, 0,
        buttonStripVisible ? SCREEN_MAX_X - buttonStripWidth - 1 : SCREEN_MAX_X - 1,
        SCREEN_MAX_Y - 1);
#else
    display_->setActiveWindow(0, 0, SCREEN_MAX_X - 1, SCREEN_MAX_Y - 1);
    fn(buttonStripScreenX0);
    display_->setActiveWindow(0, 0, SCREEN_MAX_X - buttonStripWidth - 1, SCREEN_MAX_Y - 1);
#endif
}

void showButtonStrip() {
    if (buttonStripVisible) return;
    // Reset the active window to the full panel before drawing -- without
    // this, the very first call after boot inherited whatever narrower
    // window the "Draw text" step at startup had set (which deliberately
    // excludes the button-strip area, since buttons aren't shown yet at
    // that point), causing the bitmap to draw clipped/mispositioned.
    // hideButtonStrip() already does this at ITS OWN start, which is why
    // every call AFTER the first one (following a hide) worked fine --
    // only the very first show, with no prior hide to reset it, was
    // affected.
    display_->setActiveWindow(0, 0, SCREEN_MAX_X - 1, SCREEN_MAX_Y - 1);

    // 5" path (the #else branch below): builds the strip up in kSlideSteps
    // horizontal slices, entirely on-screen. The 7" PIP-2 path shows the
    // same thing -- source columns [0, w) at the right edge -- by just
    // growing the PIP window. Verified geometry (worth spelling out precisely, since
    // getting this backwards once already produced a mirrored result):
    // because this always crops from SOURCE column 0 growing (see
    // below for why), EVERY already-drawn pixel's own screen X position
    // shifts LEFT by exactly this step's own growth amount every step --
    // e.g. with "ABCDEF" revealed 3 letters at a time, after step 1
    // "ABC" sits at the panel's own right edge; after step 2, "ABC" has
    // moved 3px left, and "DEF" (the newly-revealed slice) now occupies
    // the right edge "ABC" used to sit at. So each step: (1) BTE-shift
    // the existing, already-visible content LEFT by this step's own
    // growth amount -- pure hardware SDRAM-to-SDRAM move, no SPI pixel
    // data at all (see LT7683::bteHorizontalShift()'s own comment for
    // how this avoids the overlapping-copy risk bteScrollShift() also
    // avoids), (2) draw ONLY the newly-revealed slice, at the panel's
    // own right edge, via the proven MCU-write BTE path -- not the whole
    // accumulated width from scratch.
#ifdef DISPLAY_7INCH
    // Slide in by growing the PIP-2 window -- the status LEDs are already
    // up to date in the strip layer (see updateStatusLed()).
    for (int i = 1; i <= kSlideSteps; ++i) {
        showStripPip(stripVisibleWidth(i));
        sleep_ms(kSlideStepDelayMs);
    }
#else
    std::uint16_t segW[kSlideSteps];
    computeSegmentWidths(segW);

    std::uint16_t accumWidth = 0;
    for (int i = 0; i < kSlideSteps; ++i) {
        const std::uint16_t prevAccumWidth = accumWidth;
        accumWidth = static_cast<std::uint16_t>(accumWidth + segW[i]);
        if (prevAccumWidth > 0) {
            // Shift the existing [SCREEN_MAX_X-prevAccumWidth,
            // SCREEN_MAX_X) block left by segW[i].
            display_->bteHorizontalShift(
                static_cast<std::int16_t>(SCREEN_MAX_X - prevAccumWidth), 0,
                prevAccumWidth, buttonStripHeight,
                -static_cast<std::int16_t>(segW[i]));
        }
        // The newly-revealed slice -- source columns [prevAccumWidth,
        // accumWidth) -- lands at the panel's own right edge, in the
        // space the shift above just vacated.
        const auto sliceDestX = static_cast<std::int16_t>(SCREEN_MAX_X - segW[i]);
        display_->bteMcuWriteBitmap(sliceDestX, 0, segW[i], buttonStripHeight,
                                    buttonStripWidth,
                                    buttonStripPixels.data() + prevAccumWidth);
        sleep_ms(kSlideStepDelayMs);
    }

    setStatusLed(display_, StatusLed::Usb, usbLedOn, buttonStripScreenX0, buttonStripHeight);
    setStatusLed(display_, StatusLed::Pil, pilLedOn, buttonStripScreenX0, buttonStripHeight);
#endif

    // Now narrow the active window/text area back and let Screen reflow
    // into it.
    display_->setActiveWindow(0, 0, SCREEN_MAX_X - buttonStripWidth - 1, SCREEN_MAX_Y - 1);
    screen_->setTextWidth(SCREEN_MAX_X - buttonStripWidth);

    buttonStripVisible = true;
}

void hideButtonStrip() {
    if (!buttonStripVisible) return;

    // 5" path (the #else branch below). Verified geometry (see
    // showButtonStrip()'s own comment for the
    // full "ABCDEF" walkthrough this mirrors): the KEPT portion (source
    // columns [0, keepWidth), i.e. everything except the trailing slice
    // about to disappear) shifts RIGHT by this step's own shrink amount,
    // uncovering blank space on the left -- the source bitmap's own
    // trailing (right) edge is what actually falls off the panel's edge
    // each step, not its leading edge. Each step now: (1) BTE-shift the
    // kept region right -- pure hardware SDRAM-to-SDRAM move, no SPI
    // pixel data (see LT7683::bteHorizontalShift()'s own comment), (2)
    // clear only the newly-vacated sliver on the left -- no redraw of
    // anything else needed, since the shift already moved the real
    // pixels to their correct new position.
    display_->setActiveWindow(0, 0, SCREEN_MAX_X - 1, SCREEN_MAX_Y - 1);

#ifdef DISPLAY_7INCH
    // Slide out by shrinking the PIP-2 window -- the panel underneath was
    // never touched, so nothing needs redrawing afterwards.
    for (int i = kSlideSteps - 1; i >= 0; --i) {
        showStripPip(i == 0 ? 0 : stripVisibleWidth(i));
        sleep_ms(kHideStepDelayMs);
    }
#else
    std::uint16_t segW[kSlideSteps];
    computeSegmentWidths(segW);

    std::uint16_t accumWidth = buttonStripWidth;  // currently-visible block's width
    for (int i = kSlideSteps - 1; i >= 0; --i) {
        const std::uint16_t lastW = segW[i];
        const auto keepWidth = static_cast<std::uint16_t>(accumWidth - lastW);
        const auto oldX = static_cast<std::int16_t>(SCREEN_MAX_X - accumWidth);
        if (keepWidth > 0) {
            // Shift the [oldX, oldX+keepWidth) block right by lastW.
            display_->bteHorizontalShift(oldX, 0, keepWidth, buttonStripHeight,
                                         static_cast<std::int16_t>(lastW));
        }
        // The vacated sliver, now on the left -- [oldX, oldX+lastW).
        display_->fillRect(oldX, 0, static_cast<std::int16_t>(lastW),
                           buttonStripHeight, 0x0000);
        accumWidth = keepWidth;
        sleep_ms(kHideStepDelayMs);
    }
#endif

    // Widen the active window to the full panel, then let Screen reflow
    // into it -- full()'s own hardware clear wipes the button pixels, and
    // the wider layout naturally uses that space for text afterward.
    screen_->setTextWidth(SCREEN_MAX_X);

    // setTextWidth() above updates Screen's own row/column bookkeeping
    // either way, but its reflow() only actually redraws anything if
    // Screen isn't suspended -- and it IS suspended whenever Plotter
    // output (see plotterview.h) is what's showing. So in that case,
    // repaint the plot ourselves: otherwise the space the button bitmap
    // just occupied would be left showing stale button pixels instead of
    // the drawing. Only that strip actually needs it -- the rest of the
    // screen was never touched by the button bitmap in the first place.
    // 5" only -- on 7" the strip is a PIP-2 overlay that never touched
    // the panel.
#ifndef DISPLAY_7INCH
    if (plotterview_isActive()) {
        plotterview_redrawRegion(static_cast<std::int16_t>(buttonStripScreenX0), 0,
                                 static_cast<std::int16_t>(buttonStripWidth),
                                 static_cast<std::int16_t>(buttonStripHeight));
    }
#endif

    buttonStripVisible = false;
}

// Updates a status LED's cached state and, only if the button strip is
// currently visible, redraws the on-screen circle immediately. If the
// strip is hidden, the new value is simply cached -- showButtonStrip()
// re-applies it from usbLedOn/pilLedOn the next time the strip is shown,
// so nothing is lost, it just isn't drawn into a currently-blank area.
void updateStatusLed(StatusLed which, bool& cached, bool newState) {
    if (newState == cached) return;
    cached = newState;
#ifdef DISPLAY_7INCH
    // Always keep the strip layer current -- it's what PIP-2 shows the
    // next time the strip slides in.
    drawOnStrip([&](std::uint16_t x0) {
        setStatusLed(display_, which, cached, x0, buttonStripHeight);
    });
#else
    if (buttonStripVisible) {
        // Same active-window reset as boardui_handleTap()/handleRelease()
        // above, for the same reason -- this runs independently of
        // showButtonStrip()'s own flow (triggered by a live USB/PILBOX
        // state change, via boardui_poll()), so it can run at any time
        // the strip is visible -- including while Screen's own text area
        // has the active window narrowed to exclude the strip, same as
        // the button-press case.
        display_->setActiveWindow(0, 0, SCREEN_MAX_X - 1, SCREEN_MAX_Y - 1);
        setStatusLed(display_, which, cached, buttonStripScreenX0, buttonStripHeight);
        // Restore it afterward -- left at "full panel", a later
        // Screen::full() (e.g. on menu close) would clear the whole
        // display instead of just its own text area. Same bug/fix as
        // boardui_handleTap()'s own matching restore.
        display_->setActiveWindow(0, 0, SCREEN_MAX_X - buttonStripWidth - 1, SCREEN_MAX_Y - 1);
    }
#endif
}

// ── Top-left-corner info box ────────────────────────────────────────────────
//
// Touching the top-left corner of the screen shows a summary of the current
// config. Reuses Screen's own suspend()/resume() -- same mechanism the menu
// already relies on -- so incoming HP-41 stream bytes still update the text
// buffer while the box is up, and resume() catches the display back up the
// moment it's dismissed, instead of losing anything.
bool infoBoxVisible = false;
absolute_time_t infoBoxHideDeadline;
constexpr std::uint32_t kInfoBoxShowMs = 5000;
constexpr std::uint16_t kInfoCornerSize = 120;  // touch hit-zone: top-left NxN

bool isInfoCornerTouch(std::uint16_t x, std::uint16_t y) {
    return x < kInfoCornerSize && y < kInfoCornerSize;
}

// Named color if it matches one of the ColorPicker menu's presets,
// otherwise falls back to the raw hex value.
const char* colorName(std::uint16_t c) {
    switch (c) {
        case 0xFFFF: return "White";
        case 0xFFE0: return "Yellow";
        case 0x07E0: return "Green";
        case 0x07FF: return "Cyan";
        case 0xF800: return "Red";
        default:     return nullptr;
    }
}

void showInfoBox() {
    if (infoBoxVisible) return;

    constexpr int boxW = 400, boxH = 220;
    constexpr int boxX = (SCREEN_MAX_X - boxW) / 2;
    constexpr int boxY = (SCREEN_MAX_Y - boxH) / 2;

#ifdef DISPLAY_7INCH
    // Draws to the PIP overlay's own separate SDRAM layer instead of
    // pausing Screen -- same reasoning, and the exact same mechanism, as
    // UiDialog's own menu system (see uidialog.hpp's handleButton() and
    // close(), which do this identically): the live main window (Screen's
    // own text, or a live plot -- see plotterview.cpp) keeps updating in
    // real time underneath, since it's never actually paused, just
    // visually covered wherever this box's own rectangle sits. Before
    // this fix, screen_->suspend() here meant nothing new showed up on
    // screen until the box closed (up to kInfoBoxShowMs later) even
    // though HP-IL itself kept being processed completely normally the
    // whole time underneath -- not a genuine bus-level stall, but looked
    // exactly like one from the screen never actually changing.
    //
    // Reuses the SAME PIP-1 hardware window the menu itself uses (see
    // LT7683::showPipOverlay()) rather than a second one -- safe since
    // the two are never actually shown at the same time (boardui_handleTap()
    // only ever reaches showInfoBox() when neither the menu nor its own
    // touch-dismissible result states are currently up).
    display_->beginOverlayDraw();
#else
    screen_->suspend();
#endif

    MenuFrame::draw(display_, boxX, boxY, boxW, boxH);

    display_->txtColor(0xFFFF, 0x0000);
    char buf[64];

    display_->txtSize(1);
    std::snprintf(buf, sizeof(buf), "HIPI-Board %s", version_);
    display_->txtSetCursor(boxX + 20, boxY + 16);
    display_->txtWrite(buf);

    display_->txtSize(0);
    int y = boxY + 56;
    constexpr int lineStep = 24;

    if (hpilDevices > 0) {
        std::snprintf(buf, sizeof(buf), "Devices: %d", hpilDevices);
    } else {
        std::snprintf(buf, sizeof(buf), "Devices: (not configured)");
    }
    display_->txtSetCursor(boxX + 20, y); display_->txtWrite(buf); y += lineStep;

    std::snprintf(buf, sizeof(buf), "File: %s",
                  config.filename().empty() ? "No media" : config.filename().c_str());
    display_->txtSetCursor(boxX + 20, y); display_->txtWrite(buf); y += lineStep;

    std::snprintf(buf, sizeof(buf), "Trace: %s",
                  config.extTrace() ? "Extended" : (config.trace() ? "On" : "Off"));
    display_->txtSetCursor(boxX + 20, y); display_->txtWrite(buf); y += lineStep;

    std::snprintf(buf, sizeof(buf), "Font size: %u", config.fontSize());
    display_->txtSetCursor(boxX + 20, y); display_->txtWrite(buf); y += lineStep;

    if (config.columns() == 0) std::snprintf(buf, sizeof(buf), "Columns: Auto");
    else                       std::snprintf(buf, sizeof(buf), "Columns: %u", config.columns());
    display_->txtSetCursor(boxX + 20, y); display_->txtWrite(buf); y += lineStep;

    const char* cname = colorName(config.textColor());
    if (cname) std::snprintf(buf, sizeof(buf), "Text color: %s", cname);
    else       std::snprintf(buf, sizeof(buf), "Text color: 0x%04X", config.textColor());
    display_->txtSetCursor(boxX + 20, y); display_->txtWrite(buf); y += lineStep;

#ifdef DISPLAY_7INCH
    display_->endOverlayDraw();
    screen_->reassertRenderState();
    display_->showPipOverlay(boxX, boxY, boxW, boxH);
#endif

    infoBoxVisible = true;
    infoBoxHideDeadline = make_timeout_time_ms(kInfoBoxShowMs);
}

void hideInfoBox() {
    if (!infoBoxVisible) return;
#ifdef DISPLAY_7INCH
    display_->hidePipOverlay();
#else
    screen_->resume();  // catches up on anything the HP-41 stream sent meanwhile
#endif
    infoBoxVisible = false;
}

// ── Devices list dialog (bottom-left corner tap) ────────────────────────
// Shows the same result hipi_test()'s boot-time self-check log does --
// address, name, SAI, and enabled/disabled status for every device -- but
// as an on-screen, scrollable dialog instead of USB log text. Reuses
// hipi_enumerateDevices() (see hipi.h) directly, so it re-runs the exact
// same AAD/TAD/SAI/SDI exchange live, entirely via dev->hpil() and never
// touching the physical PIO loop. That's the same tradeoff hipi_test()
// itself carries: safe as long as nothing else is expected to be
// mid-transaction on the real bus at that moment -- calling it while a
// real controller is actively talking to us risks disrupting that
// exchange. Chosen deliberately anyway (see conversation this came from)
// over a safer read-only alternative, so the list always reflects a
// fresh, live query rather than a cached/stale one.
bool deviceListVisible = false;
int deviceListScrollOffset = 0;
std::vector<DeviceInfo> deviceListCache;

constexpr std::uint16_t kDeviceListCornerSize = 120;  // touch hit-zone: bottom-left NxN

bool isDeviceListCornerTouch(std::uint16_t x, std::uint16_t y) {
    return x < kDeviceListCornerSize && y >= static_cast<std::uint16_t>(SCREEN_MAX_Y - kDeviceListCornerSize);
}

bool isInsideDeviceListBox(std::uint16_t x, std::uint16_t y) {
    return x >= MenuFrame::X && x < MenuFrame::X + MenuFrame::W &&
           y >= MenuFrame::Y && y < MenuFrame::Y + MenuFrame::H;
}

// How many device rows fit below the title line.
// MenuFrame::RowPitch (36px) is sized for TextScale(1) -- the settings
// menu's font. The device list uses the smaller txtSize(0) (16px glyph
// height), so reusing that pitch left a lot of dead space between rows.
constexpr int kDeviceListRowPitch = 22;

// Space reserved above the rows for the "Devices" title (txtSize(1) at
// Y+16 -- roughly 32px tall, so this needs real clearance past that, not
// just past its baseline) -- too little here was exactly what let the
// first row overlap the title.
constexpr int kDeviceListRowsTopOffset = 56;

int deviceListVisibleRows() {
    // Bottom margin reserves MenuFrame::CornerRadius so the row area
    // (see drawDeviceListRows()'s fill below) never has to reach all the
    // way into the frame's rounded corners.
    return (MenuFrame::H - kDeviceListRowsTopOffset - MenuFrame::CornerRadius) / kDeviceListRowPitch;
}

// Redraws just the row area (not the frame/title) -- used both for the
// initial display and every scroll step, so scrolling doesn't need to
// redraw the whole dialog each time.
void drawDeviceListRows() {
    const int rowsY = MenuFrame::Y + kDeviceListRowsTopOffset;
    // Inset by CornerRadius on every side (not just BorderThickness) --
    // this fill is a plain rectangle, not a rounded one, so anything
    // closer to the corners than the frame's own CornerRadius would
    // paint squarely over the curve where the yellow border is meant to
    // show through, blacking out the rounded corners. A real test showed
    // exactly that on the two corners this fill's own bottom edge
    // reached (top was never close enough to be affected).
    const int rowsBottom = MenuFrame::Y + MenuFrame::H - MenuFrame::CornerRadius;
    const int fillX = MenuFrame::X + MenuFrame::CornerRadius;
    const int fillW = MenuFrame::W - 2 * MenuFrame::CornerRadius;
    display_->fillRect(static_cast<std::int16_t>(fillX),
                       static_cast<std::int16_t>(rowsY - 6),
                       fillW,
                       static_cast<std::int16_t>(rowsBottom - (rowsY - 6)),
                       0x0000);

    display_->txtColor(0xFFFF, 0x0000);
    display_->txtSize(0);
    const int visibleRows = deviceListVisibleRows();
    int y = rowsY;
    char buf[64];
    for (int i = 0; i < visibleRows; ++i) {
        const std::size_t idx = static_cast<std::size_t>(deviceListScrollOffset + i);
        if (idx >= deviceListCache.size()) break;
        const DeviceInfo& info = deviceListCache[idx];
        if (info.addr < 0) {
            std::snprintf(buf, sizeof(buf), "%6s  %-10s [%c]",
                          "--", info.devName, info.enabled ? 'X' : ' ');
        } else {
            std::snprintf(buf, sizeof(buf), "addr%2d  %-10s0x%02X [%c]",
                          info.addr, info.devName, info.sai, info.enabled ? 'X' : ' ');
        }
        display_->txtSetCursor(MenuFrame::X + 20, y);
        display_->txtWrite(buf);
        y += kDeviceListRowPitch;
    }

    // Simple scroll hints -- "^" if there's more above, "v" if there's
    // more below -- drawn in the frame's own border area (top/bottom
    // right corner of the box) rather than stealing a row from the list.
    display_->txtSetCursor(MenuFrame::X + MenuFrame::W - 30, MenuFrame::Y + 16);
    display_->txtWrite(deviceListScrollOffset > 0 ? "^" : " ");
    const bool moreBelow = deviceListScrollOffset + visibleRows <
                           static_cast<int>(deviceListCache.size());
    display_->txtSetCursor(MenuFrame::X + MenuFrame::W - 30, rowsBottom - kDeviceListRowPitch);
    display_->txtWrite(moreBelow ? "v" : " ");
}

void showDeviceList() {
    if (deviceListVisible) return;

    deviceListCache = hipi_enumerateDevices();
    deviceListScrollOffset = 0;

    // Same PIP-vs-suspend split, same reasoning, as showInfoBox() just
    // above -- see its own comment for the full story.
#ifdef DISPLAY_7INCH
    display_->beginOverlayDraw();
#else
    screen_->suspend();
#endif

    MenuFrame::draw(display_, MenuFrame::X, MenuFrame::Y, MenuFrame::W, MenuFrame::H);

    display_->txtColor(0xFFFF, 0x0000);
    display_->txtSize(1);
    display_->txtSetCursor(MenuFrame::X + 20, MenuFrame::Y + 16);
    display_->txtWrite("Devices");
    display_->txtSize(0);

    drawDeviceListRows();

#ifdef DISPLAY_7INCH
    display_->endOverlayDraw();
    screen_->reassertRenderState();
    display_->showPipOverlay(MenuFrame::X, MenuFrame::Y, MenuFrame::W, MenuFrame::H);
#endif

    deviceListVisible = true;
}

void hideDeviceList() {
    if (!deviceListVisible) return;
#ifdef DISPLAY_7INCH
    display_->hidePipOverlay();
#else
    screen_->resume();  // catches up on anything the HP-41 stream sent meanwhile
#endif
    deviceListVisible = false;
}

void scrollDeviceList(bool down) {
    const int visibleRows = deviceListVisibleRows();
    const int maxOffset = std::max(0, static_cast<int>(deviceListCache.size()) - visibleRows);
    // down=true means the finger moved downward (top-to-bottom) -- on a
    // touchscreen, that drags the CONTENT down with it, revealing rows
    // further UP the list (offset decreases). A swipe upward (down=false)
    // does the opposite: content follows the finger up, revealing rows
    // further DOWN the list (offset increases). Standard "content follows
    // the finger" touchscreen convention -- inverted from this was the
    // actual bug: a real test showed swiping up from offset 0 (nothing
    // above it yet to reveal by going the other way) visibly did nothing.
    int newOffset = deviceListScrollOffset + (down ? -1 : 1);
    if (newOffset < 0) newOffset = 0;
    if (newOffset > maxOffset) newOffset = maxOffset;
    if (bTrace) {
        mLOGF("TOUCH", "scrollDeviceList down=%d: devices=%zu visibleRows=%d maxOffset=%d %d->%d",
             down, deviceListCache.size(), visibleRows, maxOffset, deviceListScrollOffset, newOffset);
    }
    if (newOffset != deviceListScrollOffset) {
        deviceListScrollOffset = newOffset;
        // Called from boardui_handleSwipe() -- a separate call path from
        // showDeviceList() above, needing its own explicit PIP wrapping
        // for the same reason dismissLoopbackResult()/dismissI2CScanResult()
        // do in uidialog.hpp (see their own comments there): nothing else
        // wraps this call in one.
#ifdef DISPLAY_7INCH
        display_->beginOverlayDraw();
#endif
        drawDeviceListRows();
#ifdef DISPLAY_7INCH
        display_->endOverlayDraw();
        screen_->reassertRenderState();
#endif
    }
}

}  // namespace

// ── Splash screen ────────────────────────────────────────────────────────

void showSplashScreen(DisplayDriver* display, const char* version,
                      std::uint32_t durationMs) {
    // setActiveWindow() hasn't been called yet this early in boot, so the
    // active window is still at its undefined power-on state. rect/circle
    // draws (which fillRoundRect is built from) are clipped to that window,
    // so without this the corners/edges render incorrectly.
    //
    // Panel size read from the driver itself (display->width()/height(),
    // 800x480 or 1024x600 depending on DISPLAY_5INCH/7INCH -- see
    // display_config.h) rather than hardcoded -- this used to hardcode
    // 800/479 directly, which meant the active window (and thus every
    // draw below, clipped to it) stayed stuck at the 5" panel's size even
    // when built for the 7" LT7683 panel: only the upper-left 800x480
    // corner of the real 1024x600 screen ever got used.
    const int panelW = display->width();
    const int panelH = display->height();
    display->setActiveWindow(0, 0, static_cast<std::uint16_t>(panelW - 1),
                              static_cast<std::uint16_t>(panelH - 1));

    // Own box (not MenuFrame::X/Y/W/H), sized for the logo (96x114) + text,
    // and centered on the full display -- the menu's own box is sized/
    // positioned for the touch-button layout, which doesn't apply here.
    constexpr int splashW = 400, splashH = 170;
    const int splashX = (panelW - splashW) / 2;
    const int splashY = (panelH - splashH) / 2;
    MenuFrame::draw(display, splashX, splashY, splashW, splashH);

    // Logo on the left, vertically centered in the box; if it's missing
    // from the SD card, just skip it and fall back to text-only (same
    // graceful-degradation style as the button strip in boardui_loadButtonStrip()).
    const int logoX = splashX + 20;
    std::uint16_t logoW = 0, logoH = 0;
    // Peek dimensions first so we can vertically center whatever size the
    // logo actually is.
    const bool haveDims = peekBmpDimensions("logo.bmp", logoW, logoH);
    const int logoY = splashY + (splashH - (haveDims ? logoH : 0)) / 2;
    const bool haveLogo = haveDims &&
        drawBmpAt(display, "logo.bmp", logoX, logoY, nullptr, &logoW, &logoH);

    // Text sits to the right of the logo (or at the usual left margin if
    // the logo failed to load).
    const int textX = haveLogo ? (logoX + logoW + 20) : (splashX + 20);
    const int textTop = splashY + (splashH - 90) / 2;  // ~90px tall text block

    display->txtColor(0xFFFF, 0x0000);

    display->txtSize(1);   // slightly larger title row
    display->txtSetCursor(textX, textTop);
    display->txtWrite("HIPI-Board");

    display->txtSize(0);
    display->txtSetCursor(textX, textTop + 46);
    display->txtWrite("By Thomas Fänge");

    char buf[48];
    std::snprintf(buf, sizeof(buf), "Version %s", version);
    display->txtSetCursor(textX, textTop + 70);
    display->txtWrite(buf);

    display->spiDelayMs(durationMs);
}

// ── Public API ──────────────────────────────────────────────────────────

std::uint16_t boardui_loadButtonStrip(DisplayDriver* display, const char* bmpPath) {
    display_ = display;

    LOGF("\r\n\t* Load buttons ... ");
    // alsoDraw=false: only decode and cache the pixels (buttonStripPixels)
    // for later use -- the strip itself stays off-screen until a real tap
    // first calls showButtonStrip(), which draws from that cache and sets
    // up LED state correctly on its own. Drawing it here too used to make
    // it appear briefly at boot, which is exactly what showed up during
    // the device self-check / startup wait (see hipi_test() and
    // pico_main.cpp) -- distracting from the text meant to be read there.
    if (!drawBmpRightAligned(display, bmpPath, SCREEN_MAX_X, 0,
                             &buttonStripPixels, &buttonStripWidth,
                             &buttonStripHeight, &buttonStripScreenX0,
                             /*alsoDraw=*/false)) {
        LOGF("\r\n ### Failed to draw buttons ... ");
        return 0;
    }

    // Scale vertically to fill this panel's real height -- buttons.bmp
    // was made for the 5" board's 480px-tall panel; this board's is
    // 600px (SCREEN_MAX_Y). Nearest-neighbor, not interpolated -- keeps
    // button edges crisp rather than blurring them across a soft
    // gradient, which matters more here than smooth scaling would.
    // Width is untouched -- only height needs adapting, since the strip
    // is already correctly right-aligned regardless of panel width (see
    // drawBmpRightAligned() above).
    //
    // NOTE: this scales the BITMAP itself, not the hit-test rectangles
    // in ui_buttons.hpp -- those are still hardcoded to the 5" board's
    // pixel coordinates (already flagged as a known, separate issue
    // needing its own fix) and won't line up with where the now-taller
    // buttons actually appear until they're updated too.
    if (buttonStripHeight != SCREEN_MAX_Y && buttonStripHeight > 0) {
        std::vector<std::uint16_t> scaled(
            static_cast<std::size_t>(buttonStripWidth) * SCREEN_MAX_Y);
        for (std::uint16_t y = 0; y < SCREEN_MAX_Y; ++y) {
            const std::uint16_t srcY = static_cast<std::uint16_t>(
                (static_cast<std::uint32_t>(y) * buttonStripHeight) / SCREEN_MAX_Y);
            std::memcpy(scaled.data() + static_cast<std::size_t>(y) * buttonStripWidth,
                       buttonStripPixels.data() + static_cast<std::size_t>(srcY) * buttonStripWidth,
                       static_cast<std::size_t>(buttonStripWidth) * sizeof(std::uint16_t));
        }
        buttonStripPixels = std::move(scaled);
        LOGF("(scaled %u -> %u tall) ", buttonStripHeight, SCREEN_MAX_Y);
        buttonStripHeight = SCREEN_MAX_Y;
    }

#ifdef DISPLAY_7INCH
    // Copy the strip into its own PIP-2 layer once. The layer's stride
    // must be a multiple of 4 pixels (PIP requirement), so any extra
    // columns go on the LEFT and repeat the strip's first column -- the
    // strip's right edge stays flush with the panel's right edge.
    stripStride = static_cast<std::uint16_t>((buttonStripWidth + 3) & ~3);
    stripPad = static_cast<std::uint16_t>(stripStride - buttonStripWidth);
    std::vector<std::uint16_t> padded(static_cast<std::size_t>(stripStride) * buttonStripHeight);
    for (std::uint16_t y = 0; y < buttonStripHeight; ++y) {
        const std::uint16_t* src = buttonStripPixels.data() + static_cast<std::size_t>(y) * buttonStripWidth;
        std::uint16_t* dst = padded.data() + static_cast<std::size_t>(y) * stripStride;
        std::fill(dst, dst + stripPad, src[0]);
        std::memcpy(dst + stripPad, src, static_cast<std::size_t>(buttonStripWidth) * sizeof(std::uint16_t));
    }
    drawOnStrip([&](std::uint16_t x0) {
        display_->drawBitmap565(0, 0, stripStride, buttonStripHeight, padded.data());
        setStatusLed(display_, StatusLed::Usb, usbLedOn, x0, buttonStripHeight);
        setStatusLed(display_, StatusLed::Pil, pilLedOn, x0, buttonStripHeight);
    });
    LOGF("(PIP layer, stride %u) ", stripStride);
#endif

    return buttonStripWidth;
}

void boardui_init(Screen* screen, UiDialog* dialog, const char* version) {
    screen_  = screen;
    dialog_  = dialog;
    version_ = version;

    // Shift+Ok ("EXIT") always means "leave and hide the buttons" -- see
    // UiDialog::setExitRequestedCallback()'s own comment for why this
    // needs to live here rather than inside UiDialog itself.
    dialog_->setExitRequestedCallback([]() {
        hideButtonStrip();
    });

    // Initial USB/PILBOX status LED state is picked up by the periodic
    // updateStatusLed() check in boardui_poll() (runs on the very first
    // call too), so no one-shot draw is needed here.
}

void boardui_poll() {
    // Auto-hide the info box after kInfoBoxShowMs, same idea as the button
    // strip's own countdown below.
    if (infoBoxVisible && time_reached(infoBoxHideDeadline)) {
        hideInfoBox();
    }

    // Keep usb_connected live -- it used to be a one-shot flag set only if
    // tud_mounted() became true within the first 2 seconds of boot (see
    // pico_main.cpp), and never touched again. If the host took longer
    // than that to enumerate the device, it stayed permanently false,
    // which not only left the USB status LED wrong forever, but also kept
    // LOGF() output silenced (usb_connected also gates that) even once a
    // terminal did connect. Refreshing it here every poll makes it an
    // accurate, ongoing signal instead of a boot-time snapshot.
    usb_connected = tud_mounted();

    // Deferred drive switch-off (power switch / Devices menu) -- done here,
    // in the main loop between HP-IL frames, once the drive is idle
    if (CDrive* drive = plotterview_drive()) {
        drive->servicePendingDisable();
    }

    // PC ejected the SD card while in "Connect to PC" mode -- back to
    // normal mode (done inside usbMscPollHostEject()), and update the
    // menu row if it's showing
    if (usbMscPollHostEject()) {
        dialog_->refreshUsbMscRow();
    }

    // Live USB/PILBOX connection status, checked every kStatusCheckMs
    // regardless of whether the button strip is currently visible --
    // updateStatusLed() caches the new state either way and only touches
    // the display when the strip is actually shown. Checked BEFORE the
    // auto-hide below so a state change is never missed by hiding the
    // strip in the same poll that would otherwise have drawn it.
    //
    // USB: usb_connected (now kept live above, via tud_mounted()) --
    // NOT tud_cdc_n_connected(0), which only reflects whether a terminal
    // app happens to have the debug port open right now, not whether the
    // USB cable itself is connected.
    // PILBOX: pilbox->isConnected(), i.e. PILBox_mode != TDIS -- the PC
    // app has actually put the PILBox into an active mode via a command
    // frame, not just that the underlying serial channel is open.
    if (time_reached(statusCheckDeadline)) {
        statusCheckDeadline = make_timeout_time_ms(kStatusCheckMs);
        updateStatusLed(StatusLed::Usb, usbLedOn, usb_connected);
        updateStatusLed(StatusLed::Pil, pilLedOn, pilbox && pilbox->isConnected());
    }

    // Auto-hide the strip after kButtonStripHideMs of inactivity -- but
    // never while the menu is open; you need the buttons visible to
    // navigate it.
    if (buttonStripVisible && !dialog_->isOpen() &&
        time_reached(buttonStripHideDeadline)) {
        hideButtonStrip();
    }
}

void boardui_handleTap(std::uint16_t x, std::uint16_t y) {
#ifdef DISPLAY_7INCH
    // The view-switch splash owns the PIP overlay for its 1.5 s -- ignore
    // taps meanwhile, so the menu, info box or device list
    // (which all use the same overlay) can't take it over and then be
    // hidden by the splash's own timeout. See plotterview.cpp.
    if (plotterview_isSplashVisible()) return;
#endif
    if (dialog_ != nullptr && dialog_->isShowingLoopbackResult()) {
        // Any touch anywhere dismisses the loopback test's own result
        // dialog -- same "any touch while it's up" pattern as
        // infoBoxVisible below, just routed through UiDialog since this
        // result is one of its own menu states (see uidialog.hpp's
        // openLoopbackConfirm()/runLoopbackTest()) rather than a
        // separate corner-tap box like infoBox/deviceList are. Checked
        // first, same priority reasoning as infoBoxVisible/
        // deviceListVisible below -- consumed here, not treated as a
        // button-strip wake/press or corner-tap.
        dialog_->dismissLoopbackResult();
    } else if (dialog_ != nullptr && dialog_->isShowingI2CScanResult()) {
        // Same pattern as isShowingLoopbackResult() just above -- see
        // uidialog.hpp's own runI2CScan()/dismissI2CScanResult().
        dialog_->dismissI2CScanResult();
    } else if (infoBoxVisible) {
        // Any touch while it's up dismisses it early -- consumed here, not
        // treated as a button-strip wake/press or another corner-tap.
        hideInfoBox();
    } else if (deviceListVisible) {
        // Only a tap OUTSIDE the dialog closes it -- a tap inside is
        // simply ignored (scrolling is via the vertical swipe gesture,
        // not tap, and there's nothing else to act on in here yet).
        if (!isInsideDeviceListBox(x, y)) {
            hideDeviceList();
        }
    } else if (isInfoCornerTouch(x, y)) {
        showInfoBox();
    } else if (isDeviceListCornerTouch(x, y)) {
        showDeviceList();
    } else if (!dialog_->isOpen() &&
               plotterview_tapeHitTest(x, y) == TapeHotspot::Open) {
        // Tape view: cassette window or OPEN button -> file picker.
        // The button strip is needed to navigate the list, so show it too
        // (it stays up while the menu is open).
        MTRC_LOGF("tape hotspot OPEN (%u,%u) -> file picker", x, y);
        dialog_->openFilePickerFromTape();
        showButtonStrip();
        buttonStripHideDeadline = make_timeout_time_ms(kButtonStripHideMs);
    } else if (!dialog_->isOpen() &&
               plotterview_tapeHitTest(x, y) == TapeHotspot::Power) {
        // Tape view: power switch -> enable/disable TFDRIVE, saved like the
        // Devices menu does. The switch and POWER LED follow on the next
        // plotterview_poll().
        // Switching off waits until the drive is idle (see
        // CDrive::servicePendingDisable(), serviced in boardui_poll()), so
        // the switch/LED only change once it's actually off.
        if (CDrive* drive = plotterview_drive()) {
            const bool wanted = drive->requestEnabled(!drive->wantedEnabled());
            config.setDeviceEnabled(drive->name(), wanted);
            LOGF("\r\n * %s %s (power switch)", drive->name(),
                 wanted ? "enabled" : "switching off");
        }
    } else {
        // Only a touch actually within the button strip's own screen
        // region wakes/keeps it up -- a tap elsewhere (e.g. the left side,
        // reserved for the swipe gesture -- see boardui_handleSwipe()) no
        // longer wakes it "blindly" the way any touch anywhere used to.
        // hitTestButton() still works regardless of visibility (it just
        // computes against the strip's current screen position/height,
        // not whether anything is actually drawn there yet), so a tap
        // within the strip's region wakes it even while nothing is drawn
        // there yet.
        const bool inStripZone = (x >= buttonStripScreenX0);
        if (bTrace) {
            mLOGF("TOUCH", "tap (%u,%u) inStripZone=%d stripVisible=%d",
                 x, y, inStripZone, buttonStripVisible);
        }
        if (!inStripZone) return;

        const bool wasHidden = !buttonStripVisible;
        if (wasHidden) {
            showButtonStrip();
        }
        buttonStripHideDeadline = make_timeout_time_ms(kButtonStripHideMs);

        Button b = hitTestButton(x, y, buttonStripScreenX0, buttonStripHeight);
        // The touch that just woke the strip up is consumed by the reveal
        // itself -- don't also act on it as a press, since the user
        // couldn't see what they were touching.
        if (!wasHidden && b != Button::None) {
            pressedButton = b;
            // 5": the active window must be reset to the full panel before
            // drawing (drawOnStrip() does it) -- confirmed necessary on real
            // hardware: showButtonStrip()
            // narrows the active window (to exclude the strip's own area)
            // once it finishes, so a later redraw INTO that excluded area
            // (exactly what the press-feedback shift below does) landed
            // at the wrong screen position entirely -- reported as
            // appearing on the left side of the panel -- without this.
            // Visual feedback: redraw just this button's bitmap region
            // shifted, so it looks pressed in. The baseline/restore
            // redraws use a margin larger than the shift, sourced from the
            // same strip cache, so any overflow outside the button's own
            // rect (e.g. a shift toward the top-right sticks out past the
            // rect's top and right edges) gets cleaned up too -- not just
            // the tight rect itself.
            // drawOnStrip() also handles the active window (5") -- see its
            // comment and the one below.
            drawOnStrip([&](std::uint16_t x0) {
                redrawButtonRegion(
                    display_, buttonStripPixels.data(),
                    buttonStripWidth, buttonStripHeight,
                    x0, /*stripScreenY0=*/0, b, 0, 0,
                    kPressMargin);
                redrawButtonRegion(
                    display_, buttonStripPixels.data(),
                    buttonStripWidth, buttonStripHeight,
                    x0, /*stripScreenY0=*/0, b,
                    kPressDx, kPressDy);
            });
            // Restore the narrowed active window (excluding the strip's
            // own area) that showButtonStrip() originally set -- the
            // reset above was only meant to be temporary, for these two
            // redraws. Left at "full panel" (as it was before this fix),
            // Screen::full()'s own clearActiveWindow() -- called when the
            // menu closes just below, via dialog_->handleButton() ->
            // close() -> screen_.resume() -- would wipe the ENTIRE
            // screen instead of just its own text area, which is exactly
            // what full()'s own comment says clearActiveWindow() must
            // NOT do (confirmed on real hardware: the whole display went
            // black for several seconds on menu close, until the button
            // strip reappeared and re-hid itself). drawOnStrip() restores it.
            // The redraws above went through RA8875::drawBitmap565(), which
            // switches to graphics mode (gfxMode()) and blindly zeros
            // MWCR0 -- the same register that holds the cursor-visible
            // bit. Screen never sees this happen (these calls bypass it
            // entirely), so the blinking cursor would otherwise vanish on
            // every single button touch.
            screen_->refreshCursor();
            dialog_->handleButton(b);
        }
    }
}

void boardui_handleRelease() {
    if (pressedButton == Button::None) return;
    // The button strip may have been hidden entirely DURING the press
    // (e.g. Shift+Ok -- see UiDialog::setExitRequestedCallback()'s own
    // comment -- calls hideButtonStrip() from inside boardui_handleTap()
    // itself, before this release ever fires). Without this check, the
    // redraw below would blindly restore the pressed button's "normal"
    // look onto what should now be a blank area -- confirmed on real
    // hardware as a leftover "ghost" Ok button visible after Shift+Ok
    // hid everything else.
    if (!buttonStripVisible) {
        pressedButton = Button::None;
        return;
    }
    // Restore the button's bitmap to its normal position (with the same
    // margin, to clean up any overflow from the shift regardless of
    // direction).
    drawOnStrip([&](std::uint16_t x0) {
        redrawButtonRegion(
            display_, buttonStripPixels.data(),
            buttonStripWidth, buttonStripHeight,
            x0, /*stripScreenY0=*/0, pressedButton, 0, 0,
            kPressMargin);
    });
    // Restore the narrowed active window -- see boardui_handleTap()'s own
    // matching restore for why this matters (a later Screen::full(), on
    // menu close, would otherwise clear the whole panel instead of just
    // its own text area). drawOnStrip() restores it.
    screen_->refreshCursor();  // same MWCR0-clobber fix as on press
    pressedButton = Button::None;
}

void boardui_handleSwipe(bool forward) {
    // Ignore while the menu -- or the Devices list dialog -- is open; a
    // swipe crossing either box shouldn't also switch views underneath it.
    if (dialog_->isOpen() || deviceListVisible) {
        MTRC_LOGF("swipe forward=%d ignored (menu/dialog open)", forward);
        return;
    }
    MTRC_LOGF("swipe forward=%d -> cycling display output", forward);
    // The info box shares the PIP overlay with the switch splash -- close it
    // first, so its auto-hide timer can't later remove the splash early.
    hideInfoBox();
    plotterview_cycleOutput(forward);
}

void boardui_onMenuClosed() {
    // Only matters if the strip is actually showing (it always is while a
    // menu's open, per its own auto-hide rule) -- shortens the countdown
    // to kButtonStripQuickHideMs instead of leaving whatever's left of the
    // normal 5s inactivity window, which could be nearly the full amount
    // if the last touch was right before closing. boardui_poll()'s own
    // auto-hide check picks this up exactly like the normal timeout.
    if (buttonStripVisible) {
        buttonStripHideDeadline = make_timeout_time_ms(kButtonStripQuickHideMs);
    }
}

bool boardui_isMenuOpen() {
    return dialog_ != nullptr && dialog_->isOpen();
}

void boardui_handleVerticalSwipe(bool down) {
    if (bTrace) {
        mLOGF("TOUCH", "vertical swipe down=%d deviceListVisible=%d", down, deviceListVisible);
    }
    if (deviceListVisible) {
        scrollDeviceList(down);
    }
    // No other use for a vertical swipe currently.
}

}  // namespace hipi