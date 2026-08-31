// SPDX-License-Identifier: MIT
//
// boardui.cpp -- see boardui.h for the module overview.

// usb_serial.h must come first -- it defines the LOGF macro that
// uidialog.hpp (pulled in via boardui.h), bmp_loader.hpp, and config.hpp
// all use in inline/header function bodies. Including it later left LOGF
// undefined at the point those headers were parsed.
#include "usb_serial.h"
#include "plotterview.h"

#include "boardui.h"

#include <cstdio>
#include <vector>

#include "bmp_loader.hpp"
#include "config.hpp"
#include "hipi.h"      // DeviceInfo, hipi_enumerateDevices() -- "Devices" dialog
#include "hpil.h"      // hpilDevices
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
constexpr std::uint32_t kSlideStepDelayMs = 12;
// Hiding felt rushed at the same pace as showing -- a bit longer per
// step here specifically, so it reads as a deliberate slide rather than
// a blink.
constexpr std::uint32_t kHideStepDelayMs = 24;

// Splits buttonStripWidth into kSlideSteps horizontal slices (the last
// one absorbs whatever doesn't divide evenly) -- used by
// showButtonStrip()/hideButtonStrip()'s animation below.
void computeSegmentWidths(std::uint16_t* out) {
    const auto base = static_cast<std::uint16_t>(buttonStripWidth / kSlideSteps);
    std::uint16_t assigned = 0;
    for (int i = 0; i < kSlideSteps - 1; ++i) {
        out[i] = base;
        assigned = static_cast<std::uint16_t>(assigned + base);
    }
    out[kSlideSteps - 1] = static_cast<std::uint16_t>(buttonStripWidth - assigned);
}

void showButtonStrip() {
    if (buttonStripVisible) return;

    // Builds the strip up in kSlideSteps horizontal slices, entirely
    // on-screen -- no off-screen/layer addressing at all this time (a
    // previous attempt at staging the bitmap off-screen for speed smeared
    // it across the whole panel; see the conversation this came from).
    // Each step draws just the next slice -- a small SPI transfer, not
    // the whole bitmap -- into the fresh sliver peeking in at the panel's
    // right edge, then a single hardware BTE move (0xC2, the same
    // block-move opcode Screen::bte() already uses for its own text
    // scrolling) shifts everything assembled so far left by exactly the
    // next slice's width, making room for it. Both the draw and the move
    // only ever touch on-screen coordinates (0..SCREEN_MAX_X-1).
    std::uint16_t segW[kSlideSteps];
    computeSegmentWidths(segW);

    std::uint16_t srcOffset = 0;   // how much of the bitmap's own width has been drawn so far
    std::uint16_t accumWidth = 0;  // width of the currently-assembled on-screen block
    for (int i = 0; i < kSlideSteps; ++i) {
        const std::uint16_t w = segW[i];
        const auto destX = static_cast<std::int16_t>(SCREEN_MAX_X - w);
        display_->drawBitmap565Cropped(destX, 0, w, buttonStripHeight,
                                       buttonStripWidth,
                                       buttonStripPixels.data() + srcOffset);
        srcOffset  = static_cast<std::uint16_t>(srcOffset + w);
        accumWidth = static_cast<std::uint16_t>(accumWidth + w);
        sleep_ms(kSlideStepDelayMs);

        if (i < kSlideSteps - 1) {
            const std::uint16_t nextW = segW[i + 1];
            const auto srcX = static_cast<std::uint16_t>(SCREEN_MAX_X - accumWidth);
            const auto dstX = static_cast<std::uint16_t>(srcX - nextW);
            display_->BTE(0xC2, dstX, 0, accumWidth, buttonStripHeight, srcX, 0);
            sleep_ms(kSlideStepDelayMs);
        }
    }
    // The last slice above already lands exactly at buttonStripScreenX0
    // (SCREEN_MAX_X - its own width, which equals the final resting
    // position for the last slice by construction) -- no separate final
    // full-bitmap draw needed.

    setStatusLed(display_, StatusLed::Usb, usbLedOn);
    setStatusLed(display_, StatusLed::Pil, pilLedOn);

    // Now narrow the active window/text area back and let Screen reflow
    // into it.
    display_->setActiveWindow(0, 0, SCREEN_MAX_X - buttonStripWidth - 1, SCREEN_MAX_Y - 1);
    screen_->setTextWidth(SCREEN_MAX_X - buttonStripWidth);

    buttonStripVisible = true;
}

void hideButtonStrip() {
    if (!buttonStripVisible) return;

    // Real hardware slide, not a wipe: each step moves the remaining
    // visible block one slice further right (source and destination
    // DO overlap here, since dest > src), then blackens the sliver that
    // move newly exposed. An earlier attempt at exactly this used BTE
    // opcode 0xC2 ("Move BTE in Positive Direction") and produced
    // repeated ghost fragments -- the datasheet explains why: for an
    // overlapping move where destination > source, the copy has to scan
    // from the END backward (like memmove choosing its direction based
    // on which end overlaps), or it overwrites source bytes before
    // they've been read. That's opcode 0xC3 ("Negative Direction"),
    // confirmed correct in a byte-level simulation that reproduced the
    // exact ghosting bug with 0xC2 and a clean shift with 0xC3.
    // showButtonStrip()'s own leftward shifts (dest < src) are the
    // opposite overlap case, correctly handled by 0xC2 already -- that's
    // also why show never showed this problem.
    display_->setActiveWindow(0, 0, SCREEN_MAX_X - 1, SCREEN_MAX_Y - 1);

    std::uint16_t segW[kSlideSteps];
    computeSegmentWidths(segW);

    std::uint16_t accumWidth = buttonStripWidth;  // currently-visible block's width
    for (int i = kSlideSteps - 1; i >= 0; --i) {
        const std::uint16_t lastW = segW[i];
        const auto keepWidth = static_cast<std::uint16_t>(accumWidth - lastW);
        const auto srcX = static_cast<std::uint16_t>(SCREEN_MAX_X - accumWidth);
        if (keepWidth > 0) {
            const auto dstX = static_cast<std::uint16_t>(SCREEN_MAX_X - keepWidth);
            // EXPERIMENTAL: passing the bottom-right corner of each
            // rectangle here instead of the top-left (unlike the
            // Positive-direction calls in showButtonStrip(), which do
            // use top-left) -- the datasheet describes Negative
            // Direction as processing "the latest data of source/
            // destination" first, which may mean it expects coordinates
            // for that end of the rectangle, not the start. Observed
            // behavior with top-left coordinates matched exactly what
            // an off-by-one/wrong-corner bug would produce (the last
            // slice never touched, one slice's worth of content lost).
            // If this doesn't fix it, fall back to the plain SPI
            // redraw approach used before -- see conversation history.
            const auto srcXEnd = static_cast<std::uint16_t>(srcX + keepWidth - 1);
            const auto dstXEnd = static_cast<std::uint16_t>(dstX + keepWidth - 1);
            const auto yEnd = static_cast<std::uint16_t>(buttonStripHeight - 1);
            display_->BTE(0xC3, dstXEnd, yEnd, keepWidth, buttonStripHeight, srcXEnd, yEnd);
        }
        // The sliver this step's shift no longer covers -- always
        // exactly lastW wide, starting right where the block used to.
        display_->fillRect(static_cast<std::int16_t>(srcX), 0,
                           lastW, buttonStripHeight, 0x0000);
        accumWidth = keepWidth;
        sleep_ms(kHideStepDelayMs);
    }

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
    if (plotterview_isActive()) {
        plotterview_redrawRegion(static_cast<std::int16_t>(buttonStripScreenX0), 0,
                                 static_cast<std::int16_t>(buttonStripWidth),
                                 static_cast<std::int16_t>(buttonStripHeight));
    }

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
    if (buttonStripVisible) {
        setStatusLed(display_, which, cached);
    }
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
    screen_->suspend();

    constexpr int boxW = 400, boxH = 220;
    constexpr int boxX = (SCREEN_MAX_X - boxW) / 2;
    constexpr int boxY = (SCREEN_MAX_Y - boxH) / 2;
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
                  config.filename().empty() ? "(none)" : config.filename().c_str());
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

    infoBoxVisible = true;
    infoBoxHideDeadline = make_timeout_time_ms(kInfoBoxShowMs);
}

void hideInfoBox() {
    if (!infoBoxVisible) return;
    screen_->resume();  // catches up on anything the HP-41 stream sent meanwhile
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
    screen_->suspend();

    deviceListCache = hipi_enumerateDevices();
    deviceListScrollOffset = 0;

    MenuFrame::draw(display_, MenuFrame::X, MenuFrame::Y, MenuFrame::W, MenuFrame::H);

    display_->txtColor(0xFFFF, 0x0000);
    display_->txtSize(1);
    display_->txtSetCursor(MenuFrame::X + 20, MenuFrame::Y + 16);
    display_->txtWrite("Devices");
    display_->txtSize(0);

    drawDeviceListRows();

    deviceListVisible = true;
}

void hideDeviceList() {
    if (!deviceListVisible) return;
    screen_->resume();  // catches up on anything the HP-41 stream sent meanwhile
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
        LOGF("\r\n[TOUCH] scrollDeviceList down=%d: devices=%zu visibleRows=%d maxOffset=%d %d->%d",
             down, deviceListCache.size(), visibleRows, maxOffset, deviceListScrollOffset, newOffset);
    }
    if (newOffset != deviceListScrollOffset) {
        deviceListScrollOffset = newOffset;
        drawDeviceListRows();
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
    display->setActiveWindow(0, 0, 799, 479);

    // Own box (not MenuFrame::X/Y/W/H), sized for the logo (96x114) + text,
    // and centered on the full display -- the menu's own box is sized/
    // positioned for the touch-button layout, which doesn't apply here.
    constexpr int splashW = 400, splashH = 170;
    constexpr int splashX = (800 - splashW) / 2;
    constexpr int splashY = (480 - splashH) / 2;
    MenuFrame::draw(display, splashX, splashY, splashW, splashH);

    // Logo on the left, vertically centered in the box; if it's missing
    // from the SD card, just skip it and fall back to text-only (same
    // graceful-degradation style as the button strip in boardui_loadButtonStrip()).
    constexpr int logoX = splashX + 20;
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
    return buttonStripWidth;
}

void boardui_init(Screen* screen, UiDialog* dialog, const char* version) {
    screen_  = screen;
    dialog_  = dialog;
    version_ = version;

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
    if (infoBoxVisible) {
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
    } else {
        // Only a touch actually within the button strip's own screen
        // region wakes/keeps it up -- a tap elsewhere (e.g. the left side,
        // reserved for the swipe gesture -- see boardui_handleSwipe()) no
        // longer wakes it "blindly" the way any touch anywhere used to.
        // hitTestButton() still works on fixed coordinates regardless of
        // visibility, so a tap within the strip's region wakes it even
        // while nothing is drawn there yet.
        const bool inStripZone = (x >= buttonStripScreenX0);
        if (bTrace) {
            LOGF("\r\n[TOUCH] tap (%u,%u) inStripZone=%d stripVisible=%d",
                 x, y, inStripZone, buttonStripVisible);
        }
        if (!inStripZone) return;

        const bool wasHidden = !buttonStripVisible;
        if (wasHidden) showButtonStrip();
        buttonStripHideDeadline = make_timeout_time_ms(kButtonStripHideMs);

        Button b = hitTestButton(x, y);
        // The touch that just woke the strip up is consumed by the reveal
        // itself -- don't also act on it as a press, since the user
        // couldn't see what they were touching.
        if (!wasHidden && b != Button::None) {
            pressedButton = b;
            // Visual feedback: redraw just this button's bitmap region
            // shifted, so it looks pressed in. The baseline/restore
            // redraws use a margin larger than the shift, sourced from the
            // same strip cache, so any overflow outside the button's own
            // rect (e.g. a shift toward the top-right sticks out past the
            // rect's top and right edges) gets cleaned up too -- not just
            // the tight rect itself.
            redrawButtonRegion(
                display_, buttonStripPixels.data(),
                buttonStripWidth, buttonStripHeight,
                buttonStripScreenX0, /*stripScreenY0=*/0, b, 0, 0,
                kPressMargin);
            redrawButtonRegion(
                display_, buttonStripPixels.data(),
                buttonStripWidth, buttonStripHeight,
                buttonStripScreenX0, /*stripScreenY0=*/0, b,
                kPressDx, kPressDy);
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
    // Restore the button's bitmap to its normal position (with the same
    // margin, to clean up any overflow from the shift regardless of
    // direction).
    redrawButtonRegion(
        display_, buttonStripPixels.data(),
        buttonStripWidth, buttonStripHeight,
        buttonStripScreenX0, /*stripScreenY0=*/0, pressedButton, 0, 0,
        kPressMargin);
    screen_->refreshCursor();  // same MWCR0-clobber fix as on press
    pressedButton = Button::None;
}

void boardui_handleSwipe(bool forward) {
    // Ignore while the menu -- or the Devices list dialog -- is open; a
    // swipe crossing either box shouldn't also switch views underneath it.
    if (dialog_->isOpen() || deviceListVisible) {
        if (bTrace) LOGF("\r\n[TOUCH] swipe forward=%d ignored (menu/dialog open)", forward);
        return;
    }
    if (bTrace) LOGF("\r\n[TOUCH] swipe forward=%d -> cycling display output", forward);
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

void boardui_handleVerticalSwipe(bool down) {
    if (bTrace) {
        LOGF("\r\n[TOUCH] vertical swipe down=%d deviceListVisible=%d", down, deviceListVisible);
    }
    if (deviceListVisible) {
        scrollDeviceList(down);
    }
    // No other use for a vertical swipe currently.
}

}  // namespace hipi