// SPDX-License-Identifier: MIT
//
// boardui.cpp -- see boardui.h for the module overview.

// usb_serial.h must come first -- it defines the LOGF macro that
// uidialog.hpp (pulled in via boardui.h), bmp_loader.hpp, and config.hpp
// all use in inline/header function bodies. Including it later left LOGF
// undefined at the point those headers were parsed.
#define MODULE "BOARDUI"

#include "usb_serial.h"
#include "sd_paths.h"
#include "analyzer.h"
#include "backlight.h"
#include "display_mirror.h"   // layers a PC viewer needs
#include "loopmap.h"
#include "devicelist.h"   // the HP-IL DEVICES list
#include "hwtest.h"
#include "mediamap.h"
#include "demo.h"
#include "plotterview.h"
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
#include "printer.h"   // CPrinter -- Printer view controls
#include "screendump.h"
#include "display_text.h"
extern std::vector<CDevice*> devices;
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
// sub-rectangle can be redrawn (e.g. shifted for "pressed" feedback).
// Filled in (from the built-in image) by
// boardui_loadButtonStrip() at boot. RGB565 (2 bytes/pixel), matching the
// display's 16bpp mode.
std::uint16_t buttonStripSrcHeight = 0;     // buttons.bmp's own height (kButtonsImage, flash)
}  // namespace (reopened below)
}  // namespace hipi
// Built-in button strip image -- see boardui_loadButtonStrip()
extern const std::uint16_t kButtonsImage[];
extern const std::uint16_t kButtonsImageWidth, kButtonsImageHeight;
// Built-in logo -- see showSplashScreen()
extern const std::uint16_t kLogoImage[];
extern const std::uint16_t kLogoImageWidth, kLogoImageHeight;
namespace hipi {
namespace {
std::uint16_t buttonStripWidth = 0, buttonStripHeight = 0, buttonStripScreenX0 = 0;

bool buttonStripVisible = false;
absolute_time_t buttonStripHideDeadline;
bool saverActive = false;   // a screen saver has the panel (boardui_saverBegin())
// A button held from the PC (boardui_remoteButton())
Button remoteButton = Button::None;
absolute_time_t remoteButtonHeard;
constexpr std::uint32_t kRemoteButtonTimeoutMs = 1000;
void releaseRemoteButton();
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

// Auto-repeat for ▲/▼ held down: after kRepeatDelayMs, the press repeats
// every kRepeatIntervalMs until the finger is lifted -- works everywhere
// the arrows scroll (text history, menus, lists, the analyzer), since it
// just repeats the button into UiDialog::handleButton().
constexpr std::uint32_t kRepeatDelayMs = 500;
constexpr std::uint32_t kRepeatIntervalMs = 200;
Button repeatButton = Button::None;
// Printer view: the key (PRINT / PAPER ADVANCE) the finger is on, let go
// in boardui_handleRelease()
PrinterHotspot printerKeyHeld = PrinterHotspot::None;
absolute_time_t repeatNext = nil_time;

// Slide animation timing for showButtonStrip()/hideButtonStrip() below --
// short enough to feel snappy, several steps so the motion actually reads
// as a slide rather than a jump.
constexpr int kSlideSteps = 8;
constexpr std::uint32_t kSlideStepDelayMs = 30;
// Hiding felt rushed at the same pace as showing -- a bit longer per
// step here specifically, so it reads as a deliberate slide rather than
// a blink.
constexpr std::uint32_t kHideStepDelayMs = 40;

// ── Button strip as a PIP-2 overlay ────────────────────────────────────
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

// Runs fn(stripX0) to draw part of the button strip (a pressed/released
// button, a status LED). stripX0 is where the strip's own column 0 is in
// the drawing target: the strip's PIP layer (x = stripPad) -- the PIP
// overlay shows the change immediately, whether the strip is visible or not.
template <typename F>
void drawOnStrip(F&& fn) {
    display_->beginLayerDraw(LT7683::kStripLayerAddr, stripStride);
    display_->setActiveWindow(0, 0, stripStride - 1, SCREEN_MAX_Y - 1);
    fn(stripPad);
    display_->endOverlayDraw();              // canvas back to the live panel
    if (screen_ != nullptr) screen_->reassertRenderState();
    display_->setActiveWindow(0, 0,
        buttonStripVisible ? SCREEN_MAX_X - buttonStripWidth - 1 : SCREEN_MAX_X - 1,
        SCREEN_MAX_Y - 1);
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

    // Slide in by growing the PIP-2 window -- the status LEDs are already
    // up to date in the strip layer (see updateStatusLed()).
    for (int i = 1; i <= kSlideSteps; ++i) {
        showStripPip(stripVisibleWidth(i));
        sleep_ms(kSlideStepDelayMs);
    }

    // Now narrow the active window/text area back and let Screen reflow
    // into it.
    display_->setActiveWindow(0, 0, SCREEN_MAX_X - buttonStripWidth - 1, SCREEN_MAX_Y - 1);
    screen_->setTextWidth(SCREEN_MAX_X - buttonStripWidth);

    buttonStripVisible = true;
}

void hideButtonStrip() {
    if (!buttonStripVisible) return;

    display_->setActiveWindow(0, 0, SCREEN_MAX_X - 1, SCREEN_MAX_Y - 1);

    // Slide out by shrinking the PIP-2 window -- the panel underneath was
    // never touched, so nothing needs redrawing afterwards.
    for (int i = kSlideSteps - 1; i >= 0; --i) {
        showStripPip(i == 0 ? 0 : stripVisibleWidth(i));
        sleep_ms(kHideStepDelayMs);
    }

    // Widen the active window to the full panel, then let Screen reflow
    // into it -- full()'s own hardware clear wipes the button pixels, and
    // the wider layout naturally uses that space for text afterward.
    screen_->setTextWidth(SCREEN_MAX_X);


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
    // Always keep the strip layer current -- it's what PIP-2 shows the
    // next time the strip slides in.
    drawOnStrip([&](std::uint16_t x0) {
        setStatusLed(display_, which, cached, x0, buttonStripHeight);
    });
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

    MenuFrame::draw(display_, boxX, boxY, boxW, boxH);
    display_->selectBuiltinFont();       // (a view may have left another font selected)

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

    // Each cassette drive's file
    for (CDevice* dev : devices) {
        if (dev->type() != DRIVE) continue;
        const std::string& f = static_cast<CDrive*>(dev)->mediaFile();
        std::snprintf(buf, sizeof(buf), "%s: %s", dev->name(), f.empty() ? "No media" : f.c_str());
        display_->txtSetCursor(boxX + 20, y); display_->txtWrite(buf); y += lineStep;
    }

    // The plotter's ERROR light, while it's lit
    if (plotter != nullptr && plotter->errorCode() != 0) {
        std::snprintf(buf, sizeof(buf), "%s: ERROR %d (%s)", plotter->name(), plotter->errorCode(),
                      CPlotter::errorText(plotter->errorCode()));
        display_->txtSetCursor(boxX + 20, y); display_->txtWrite(buf); y += lineStep;
    }

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

    display_->endOverlayDraw();
    screen_->reassertRenderState();
    plotterview_refreshCursor();       // see showDeviceList()
    display_->showPipOverlay(boxX, boxY, boxW, boxH);

    infoBoxVisible = true;
    infoBoxHideDeadline = make_timeout_time_ms(kInfoBoxShowMs);
}

void hideInfoBox() {
    if (!infoBoxVisible) return;
    display_->hidePipOverlay();
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
absolute_time_t deviceListNextRefresh = nil_time;
constexpr std::uint32_t kDeviceListRefreshMs = 500;

std::vector<DeviceInfo> deviceListCache;

constexpr std::uint16_t kDeviceListCornerSize = 120;  // touch hit-zone: bottom-left NxN

bool isDeviceListCornerTouch(std::uint16_t x, std::uint16_t y) {
    return x < kDeviceListCornerSize && y >= static_cast<std::uint16_t>(SCREEN_MAX_Y - kDeviceListCornerSize);
}

// The device list: a menu-style box (the frame with a yellow title band,
// as the cassette list), one device a row in the small font, scrolled a
// device at a time by a vertical swipe. Closed with its close button (the
// X in the title band) -- other touches don't close it.
// The devices as the list shows them: a drive being switched off (it
// waits until it's idle) already shows as off, as in Devices / On-Off
std::vector<DeviceInfo> listedDevices() {
    std::vector<DeviceInfo> v = hipi_enumerateDevices();
    for (DeviceInfo& info : v)
        for (CDevice* dev : devices)
            if (dev->type() == DRIVE && std::strcmp(dev->name(), info.devName) == 0)
                info.enabled = static_cast<CDrive*>(dev)->wantedEnabled();
    return v;
}

// The box itself: devicelist.h
int deviceListVisibleRows() { return devlist::kRows; }

// The rows again (the first display, every scroll step, a change), on the
// menu layer on 7"
void redrawDeviceListRows() {
    display_->beginOverlayDraw();
    devlist::drawRows(display_, deviceListCache, deviceListScrollOffset);
    display_->endOverlayDraw();
    screen_->reassertRenderState();
    plotterview_refreshCursor();       // (drawing moved the chip's shared text cursor)
}

void showDeviceList() {
    if (deviceListVisible) return;

    deviceListCache = listedDevices();
    deviceListScrollOffset = 0;
    deviceListNextRefresh = make_timeout_time_ms(kDeviceListRefreshMs);

    // Same PIP-vs-suspend split, same reasoning, as showInfoBox() just
    // above -- see its own comment for the full story.
    display_->beginOverlayDraw();

    devlist::drawFrame(display_);
    devlist::drawRows(display_, deviceListCache, deviceListScrollOffset);

    display_->endOverlayDraw();
    screen_->reassertRenderState();
    // Drawing the list moved the chip's (shared) text cursor into the
    // box -- put the blinking cursor back where the text stands
    plotterview_refreshCursor();
    display_->showPipOverlay(devlist::kX, devlist::kY, devlist::kW, devlist::kH);

    deviceListVisible = true;
}

void hideDeviceList() {
    if (!deviceListVisible) return;
    display_->hidePipOverlay();
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
        redrawDeviceListRows();
    }
}

// While the list is open it follows the loop: the addresses come when
// the HP-41 auto-addresses (AAU/AAD), devices are switched on and off ...
// Looked at twice a second; the rows are drawn again when something changed.

bool sameDevices(const std::vector<DeviceInfo>& a, const std::vector<DeviceInfo>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const DeviceInfo& p = a[i];
        const DeviceInfo& q = b[i];
        if (std::strcmp(p.devName, q.devName) != 0 || p.addr != q.addr || p.sai != q.sai || p.enabled != q.enabled ||
            p.extDevices != q.extDevices || p.extFirstAddr != q.extFirstAddr || std::strcmp(p.extMode, q.extMode) != 0)
            return false;
    }
    return true;
}

void refreshDeviceList() {
    if (!deviceListVisible || saverActive || !time_reached(deviceListNextRefresh)) return;
    deviceListNextRefresh = make_timeout_time_ms(kDeviceListRefreshMs);
    std::vector<DeviceInfo> now = listedDevices();
    if (sameDevices(now, deviceListCache)) return;
    deviceListCache = std::move(now);
    deviceListScrollOffset = std::min(deviceListScrollOffset, devlist::maxOffset(static_cast<int>(deviceListCache.size())));
    redrawDeviceListRows();
}

// A touch on a row's on/off box: that device switched on or off, as in
// Devices / On-Off (a drive like its power switch; saved), the rows again
void toggleListedDevice(int idx) {
    if (idx < 0 || idx >= static_cast<int>(deviceListCache.size())) return;
    for (CDevice* dev : devices) {
        if (std::strcmp(dev->name(), deviceListCache[static_cast<std::size_t>(idx)].devName) != 0) continue;
        dialog_->toggleDeviceEnabled(dev);
        break;
    }
    deviceListCache = listedDevices();
    deviceListNextRefresh = make_timeout_time_ms(kDeviceListRefreshMs);
    redrawDeviceListRows();
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
    // Panel size read from the driver itself (display->width()/height())
    // rather than hardcoded.
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
    display->selectBuiltinFont();       // (a view may have left another font selected)

    // Logo on the left, vertically centered in the box; if it's missing
    // from the SD card, just skip it and fall back to text-only (same
    // graceful-degradation style as the button strip in boardui_loadButtonStrip()).
    // Built-in logo (src/logo_image.cpp -- no SD card needed), vertically
    // centred in the box
    const int logoX = splashX + 20;
    const int logoY = splashY + (splashH - kLogoImageHeight) / 2;
    display->drawBitmap565(logoX, logoY, kLogoImageWidth, kLogoImageHeight, kLogoImage);

    // Text sits to the right of the logo
    const int textX = logoX + kLogoImageWidth + 20;
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

std::uint16_t boardui_loadButtonStrip(DisplayDriver* display) {
    display_ = display;

    LOGF("\r\n\t* Load buttons (built in) ... ");
    // The strip is built into the firmware (src/buttons_image.cpp,
    // generated from resources/buttons.bmp by scripts/bmp_to_rgb565.py),
    // so the buttons -- and with them the menu -- work without an SD card.
    // Not drawn here; showButtonStrip() draws it from the cache on the
    // first tap (drawing it at boot made it flash up during start-up).
    buttonStripWidth = kButtonsImageWidth;
    buttonStripHeight = kButtonsImageHeight;
    buttonStripScreenX0 = static_cast<std::uint16_t>(SCREEN_MAX_X - buttonStripWidth);
    // Drawn straight from the image in flash, stretched to the panel's
    // height -- buttons.bmp is 480px tall; the panel is 600px
    // (SCREEN_MAX_Y). Nearest-neighbor, not interpolated -- keeps button
    // edges crisp. Width is untouched. No copy in RAM: the strip lives in
    // its PIP-2 layer, and a pressed button is drawn again from flash
    // (redrawButtonRegion()). (A scaled copy used to stay on the heap:
    // 144 kB, a third of it.)
    //
    // NOTE: this scales the BITMAP itself, not the hit-test rectangles
    // in ui_buttons.hpp -- those are in buttons.bmp's own 480px-tall
    // coordinates.
    buttonStripSrcHeight = buttonStripHeight;
    const std::uint16_t srcHeight = buttonStripHeight;
    if (buttonStripHeight != SCREEN_MAX_Y && buttonStripHeight > 0) buttonStripHeight = SCREEN_MAX_Y;
    if (srcHeight != buttonStripHeight) LOGF("(scaled %u -> %u tall) ", srcHeight, buttonStripHeight);

    // Copy the strip into its own PIP-2 layer once. The layer's stride
    // must be a multiple of 4 pixels (PIP requirement), so any extra
    // columns go on the LEFT and repeat the strip's first column -- the
    // strip's right edge stays flush with the panel's right edge. Drawn
    // row by row through one small row buffer (no full padded copy).
    stripStride = static_cast<std::uint16_t>((buttonStripWidth + 3) & ~3);
    stripPad = static_cast<std::uint16_t>(stripStride - buttonStripWidth);
    std::vector<std::uint16_t> row(stripStride);
    drawOnStrip([&](std::uint16_t x0) {
        for (std::uint16_t y = 0; y < buttonStripHeight; ++y) {
            const std::uint16_t srcY = static_cast<std::uint16_t>(
                (static_cast<std::uint32_t>(y) * srcHeight) / buttonStripHeight);
            const std::uint16_t* src = kButtonsImage + static_cast<std::size_t>(srcY) * buttonStripWidth;
            std::fill(row.begin(), row.begin() + stripPad, src[0]);
            std::memcpy(row.data() + stripPad, src, static_cast<std::size_t>(buttonStripWidth) * sizeof(std::uint16_t));
            display_->drawBitmap565(0, static_cast<std::int16_t>(y), stripStride, 1, row.data());
        }
        setStatusLed(display_, StatusLed::Usb, usbLedOn, x0, buttonStripHeight);
        setStatusLed(display_, StatusLed::Pil, pilLedOn, x0, buttonStripHeight);
    });
    // A PC viewer needs this layer too (its status LEDs change at run time)
    displayMirror_addLayer(LT7683::kStripLayerAddr, static_cast<std::uint32_t>(stripStride) * 2UL * buttonStripHeight,
                           0, 1, 0);
    LOGF("(PIP layer, stride %u) ", stripStride);

    return buttonStripWidth;
}

void boardui_init(Screen* screen, UiDialog* dialog, const char* version) {
    if (dialog != nullptr) dialog->setVersion(version);     // System -> About
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

void boardui_showWarning(const char* title, const char* line1, const char* line2) {
    if (dialog_ != nullptr) dialog_->showWarning(title, line1, line2);
}

void boardui_poll() {
    refreshDeviceList();                 // (only while the device list is open)
    if (dialog_ != nullptr) dialog_->poll();   // File picker -> Create media: writing the file
    // Held ▲/▼: repeat (see kRepeatDelayMs). Released -> boardui_handleRelease()
    if (repeatButton != Button::None && pressedButton == repeatButton &&
        time_reached(repeatNext)) {
        repeatNext = make_timeout_time_ms(kRepeatIntervalMs);
        if (dialog_->isOpen() || !plotterview_digitizeButton(static_cast<int>(repeatButton), true))
            dialog_->handleButton(repeatButton, /*repeat=*/true);
        buttonStripHideDeadline = make_timeout_time_ms(kButtonStripHideMs);  // keep the strip up
    }
    // Hardware test page: no button strip over it (left up by the menu
    // that opened the page, or a button from hipiview) -- the page's own
    // buttons reach the right edge, and its X closes it
    if (buttonStripVisible && pressedButton == Button::None && dialog_ != nullptr && !dialog_->isOpen() &&
        plotterview_output() == DisplayOutput::HwTest && !plotterview_isSplashVisible()) {
        hideButtonStrip();
    }
    // Auto-hide the info box after kInfoBoxShowMs, same idea as the button
    // strip's own countdown below.
    if (infoBoxVisible && !saverActive && time_reached(infoBoxHideDeadline)) {
        hideInfoBox();
    }
    // Safety net: an info box / device list whose PIP-1 window something
    // else has taken away isn't open any more -- otherwise it stays
    // "open" unseen and eats every touch (only its close button helped)
    if ((infoBoxVisible || deviceListVisible) && !display_->pipState(1).on) {
        LOGF("\r\n * %s lost its window -- closed", deviceListVisible ? "Device list" : "Info box");
        infoBoxVisible = false;
        deviceListVisible = false;
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
        drive->servicePowerUp();        // STANDBY wake-up completes here if the loop is quiet
    }

    // View chosen from the HP-41 ("ESC # V <addr>"). Waits while a menu is
    // open (the user is busy there; the menu belongs to the view under
    // it) and is carried out once it closes. Wakes the screen saver first,
    // so the new view is what shows -- not the view the saver goes back to.
    if (!dialog_->isOpen()) {
        std::uint8_t addr = 0;
        if (plotterview_takeViewRequest(addr)) {
            backlight_activity();
            plotterview_showAddress(addr);
        }
    }

    // Screen dump requested from the HP-41 ("ESC # D" / "ESC # M") or the
    // Screendump menu item -- taken here, outside HP-IL frame handling.
    // "Saving screendump..." is shown for a plain dump (not for "M", which
    // is meant to be taken without anything extra on screen); "Saved <file>"
    // always, so the file name is known. The boxes use the menu's PIP layer,
    // so with a menu open they cover it; the menu is put back once the
    // message has timed out (see menuCoveredByMessage below).
    static bool menuCoveredByMessage = false;
    bool withOverlays = false;
    if (screendump_takePending(withOverlays)) {
        std::string msg;
        if (!withOverlays) {
            plotterview_showMessage("Saving screendump...");
            if (dialog_->isOpen()) menuCoveredByMessage = true;
        }
        const bool ok = screendump_save(display_, msg, withOverlays);
        if (!ok) LOGF("\r\n * Screendump failed: %s", msg.c_str());
        plotterview_showMessage(ok ? ("Saved " + msg).c_str() : msg.c_str());
        if (dialog_->isOpen()) menuCoveredByMessage = true;
    }
    // The display's text saved ("ESC # T" or Display > Text > Save as text)
    if (displayText_takePending()) {
        std::string msg;
        const bool ok = displayText_save(msg);
        if (!ok) LOGF("\r\n * Saving the display text failed: %s", msg.c_str());
        plotterview_showMessage(ok ? ("Saved " + msg).c_str() : msg.c_str());
        if (dialog_->isOpen()) menuCoveredByMessage = true;
    }
    if (menuCoveredByMessage && !plotterview_isSplashVisible()) {
        menuCoveredByMessage = false;
        if (dialog_->isOpen()) dialog_->restoreAfterMessage();
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
    // A button held from the PC that it stopped telling us about
    if (remoteButton != Button::None &&
        absolute_time_diff_us(remoteButtonHeard, get_absolute_time()) > kRemoteButtonTimeoutMs * 1000) {
        releaseRemoteButton();
    }
    if (remoteButton != Button::None) buttonStripHideDeadline = make_timeout_time_ms(kButtonStripHideMs);

    if (buttonStripVisible && !dialog_->isOpen() && !saverActive &&
        time_reached(buttonStripHideDeadline)) {
        hideButtonStrip();
    }
}

// A button on the strip pressed (a touch on it, or a key in hipiview):
// the pressed look, the action, and auto-repeat for ▲/▼. The strip must be
// showing. boardui_handleRelease() lets go of it.
namespace {
void pressStripButton(Button b) {
    pressedButton = b;
    // Visual feedback: redraw just this button's bitmap region
    // shifted, so it looks pressed in. The baseline/restore
    // redraws use a margin larger than the shift, sourced from the
    // same image (flash), so any overflow outside the button's own
    // rect (e.g. a shift toward the top-right sticks out past the
    // rect's top and right edges) gets cleaned up too -- not just
    // the tight rect itself.
    // drawOnStrip() draws into the strip's own layer and restores the
    // active window afterwards.
    drawOnStrip([&](std::uint16_t x0) {
        redrawButtonRegion(
            display_, kButtonsImage, buttonStripSrcHeight,
            buttonStripWidth, buttonStripHeight,
            x0, /*stripScreenY0=*/0, b, 0, 0,
            kPressMargin);
        redrawButtonRegion(
            display_, kButtonsImage, buttonStripSrcHeight,
            buttonStripWidth, buttonStripHeight,
            x0, /*stripScreenY0=*/0, b,
            kPressDx, kPressDy);
    });
    // The redraws above went through drawBitmap565(), which switches to
    // graphics mode -- Screen never sees that, so the blinking cursor
    // would otherwise vanish on every button touch.
    plotterview_refreshCursor();
    // Digitizing (no menu open): OK = ENTER, Up/Down move the marker, X cancels
    if (dialog_->isOpen() || !plotterview_digitizeButton(static_cast<int>(b), false))
        dialog_->handleButton(b);
    if (b == Button::Up || b == Button::Down) {
        repeatButton = b;                       // see boardui_poll()
        repeatNext = make_timeout_time_ms(kRepeatDelayMs);
    }
}

// Printer view: a touch on one of its controls. The switches and keys
// act like the real ones; the switch positions are saved (CONFIG.TXT),
// power like the Devices menu does. The picture follows in
// plotterview_poll().
void handlePrinterTouch(const PrinterHit& hit) {
    CPrinter* prt = plotterview_printer();
    if (prt == nullptr) return;
    switch (hit.spot) {
        case PrinterHotspot::Power: {
            const bool on = !prt->enabled();
            prt->setEnabled(on);
            config.setDeviceEnabled(prt->name(), on);
            break;
        }
        case PrinterHotspot::Mode:
            prt->setMode(static_cast<PrinterMode>(hit.value));
            config.setPrinterMode(static_cast<std::uint8_t>(hit.value));
            break;
        case PrinterHotspot::Intensity:
            prt->setIntensity(static_cast<std::uint8_t>(hit.value));
            config.setPrinterIntensity(static_cast<std::uint8_t>(hit.value));
            break;
        case PrinterHotspot::Print:
            plotterview_printerPrint(true);
            printerKeyHeld = hit.spot;
            break;
        case PrinterHotspot::Advance:
            plotterview_printerAdvance(true);
            printerKeyHeld = hit.spot;
            break;
        default:
            break;
    }
}

}  // namespace

// A tap on the media map (mediamap.h): true if it hit something
static bool mediaMapTap(int x, int y) {
    const MediaMapTap t = mediamap_tap(x, y);
    if (t == MediaMapTap::Close) plotterview_leaveMediaMap();
    return t != MediaMapTap::None;
}

void boardui_handleTap(std::uint16_t x, std::uint16_t y) {
    // The demo: any touch stops it (demo.h)
    if (demo_running()) { demo_stop(); return; }
    // A dimmed screen: this touch only wakes it (backlight.h)
    if (backlight_wakeByTouch()) return;
    // The view-switch splash owns the PIP overlay for its 1.5 s -- ignore
    // taps meanwhile, so the menu, info box or device list
    // (which all use the same overlay) can't take it over and then be
    // hidden by the splash's own timeout. See plotterview.cpp.
    if (plotterview_isSplashVisible()) return;
    // The button strip's zone (right edge) belongs to the strip -- a view's
    // own touch areas under it (loop map cards reach the edge) don't get
    // those touches, or OK on the strip would open a card instead of the menu
    const bool stripZone = x >= buttonStripScreenX0;
    // Printer view controls (nothing while a menu is open)
    const PrinterHit printerHit = (dialog_ != nullptr && dialog_->isOpen()) || stripZone
                                      ? PrinterHit{} : plotterview_printerHitTest(x, y);
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
    } else if (dialog_ != nullptr && dialog_->isShowingWarning()) {
        // A warning box (UiDialog::showWarning()): any touch closes it
        dialog_->dismissWarning();
    } else if (dialog_ != nullptr && dialog_->wantsTouch() && !stripZone) {
        // File picker -> Create media...: its keyboard and buttons
        dialog_->handleTap(x, y);
    } else if (infoBoxVisible) {
        // Any touch while it's up dismisses it early -- consumed here, not
        // treated as a button-strip wake/press or another corner-tap.
        hideInfoBox();
    } else if (deviceListVisible) {
        // Only its close button closes it (a touch beside the box used to,
        // which made a long list easy to lose); a row's on/off box switches
        // that device; scrolling is a vertical swipe
        if (devlist::isClose(x, y)) hideDeviceList();
        else toggleListedDevice(devlist::checkboxAt(x, y, deviceListScrollOffset,
                                                    static_cast<int>(deviceListCache.size())));
    } else if (!dialog_->isOpen() && plotterview_output() == DisplayOutput::CharTable) {
        plotterview_leaveCharTable();         // the character table: any touch closes it
    } else if (!dialog_->isOpen() && plotterview_output() == DisplayOutput::HwTest && hwtest_tap(x, y)) {
        // Hardware test: its buttons (checked before the corners). Its
        // buttons reach the right edge (and its close button sits there):
        // they get the touch unless the strip is showing -- a touch there
        // that hits none of them still wakes the strip.
    } else if (!dialog_->isOpen() && !(stripZone && buttonStripVisible) &&
               plotterview_output() == DisplayOutput::MediaMap && mediaMapTap(x, y)) {
        // Media map: a ring -> that file; the X in the title bar -> back
    } else if (!dialog_->isOpen() && !stripZone && plotterview_output() == DisplayOutput::LoopMap &&
               loopmap_tap(x, y)) {
        // Loop map: a card -> its details; the details box -> closed
        // (checked before the corners: cards may sit in them)
    } else if (!dialog_->isOpen() && !stripZone && plotterview_digitizeTap(x, y)) {
        // Plotter view while the plotter waits for a point (HP-GL DP): the
        // touch is that point (checked before the corners: a point may be there)
    } else if (!dialog_->isOpen() && !stripZone && plotterview_errorLightTap(x, y)) {
        // Plotter view: the ERROR light -- what the error was (a message box)
    } else if (printerHit.spot != PrinterHotspot::None) {
        handlePrinterTouch(printerHit);
    } else if (isInfoCornerTouch(x, y) && plotterview_output() != DisplayOutput::HwTest &&
               plotterview_output() != DisplayOutput::MediaMap) {
        // (not on the Hardware test page: its buttons sit in the corners,
        // and a touch just beside one shouldn't open the box instead)
        showInfoBox();
    } else if (isDeviceListCornerTouch(x, y) && plotterview_output() != DisplayOutput::HwTest &&
               plotterview_output() != DisplayOutput::MediaMap) {
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
               plotterview_tapeHitTest(x, y) == TapeHotspot::Rewind) {
        // Tape view: REWIND button -- ignored while the drive is busy,
        // powered down or empty (see CDrive::manualRewind())
        if (CDrive* drive = plotterview_drive()) {
            if (!drive->manualRewind()) {
                MTRC_LOGF("REWIND ignored (BUSY lit, off or no tape)");
            }
        }
    } else if (!dialog_->isOpen() &&
               plotterview_tapeHitTest(x, y) == TapeHotspot::Power) {
        // Tape view: power switch -> TFDRIVE's OFF/STANDBY/ON, saved like
        // the Devices menu does. The switch and POWER LED follow on the
        // next plotterview_poll().
        // The switch picture moves at once; switching OFF itself waits
        // until the drive is idle (CDrive::servicePendingDisable(),
        // serviced in boardui_poll()), and the POWER LED goes out then.
        // Each tap moves the switch one step: OFF -> STANDBY -> ON ->
        // STANDBY -> OFF -> ...
        if (CDrive* drive = plotterview_drive()) {
            static bool towardsOn = true;       // direction through STANDBY
            DrivePower next;
            switch (drive->powerMode()) {
                case DrivePower::Off: next = DrivePower::Standby; towardsOn = true;  break;
                case DrivePower::On:  next = DrivePower::Standby; towardsOn = false; break;
                default:              next = towardsOn ? DrivePower::On : DrivePower::Off; break;
            }
            drive->setPowerMode(next);
            config.setDeviceEnabled(drive->name(), next != DrivePower::Off);
            if (next != DrivePower::Off) {
                config.setDriveStandby(next == DrivePower::Standby);
            }
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
        // Analyzer view: a tap anywhere else (not a corner, not the strip)
        // pauses / resumes it
        if (!inStripZone && !dialog_->isOpen() &&
            plotterview_output() == DisplayOutput::Analyzer) {
            analyzer_setPaused(!analyzer_paused());
            return;
        }
        if (!inStripZone) return;
        // Hardware test page: no button strip -- its own buttons reach the
        // right edge (the close X, pixel Off), and the X closes the page
        if (plotterview_output() == DisplayOutput::HwTest && !dialog_->isOpen()) return;

        const bool wasHidden = !buttonStripVisible;
        if (wasHidden) {
            showButtonStrip();
        }
        buttonStripHideDeadline = make_timeout_time_ms(kButtonStripHideMs);

        Button b = hitTestButton(x, y, buttonStripScreenX0, buttonStripHeight);
        // The touch that just woke the strip up is consumed by the reveal
        // itself -- don't also act on it as a press, since the user
        // couldn't see what they were touching.
        if (!wasHidden && b != Button::None) pressStripButton(b);
    }
}

void boardui_handleRelease() {
    repeatButton = Button::None;          // finger lifted: stop auto-repeat
    plotterview_digitizeRelease();        // (and a digitize arrow)
    if (printerKeyHeld == PrinterHotspot::Print)   plotterview_printerPrint(false);
    if (printerKeyHeld == PrinterHotspot::Advance) plotterview_printerAdvance(false);
    printerKeyHeld = PrinterHotspot::None;
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
            display_, kButtonsImage, buttonStripSrcHeight,
            buttonStripWidth, buttonStripHeight,
            x0, /*stripScreenY0=*/0, pressedButton, 0, 0,
            kPressMargin);
    });
    // Restore the narrowed active window -- see boardui_handleTap()'s own
    // matching restore for why this matters (a later Screen::full(), on
    // menu close, would otherwise clear the whole panel instead of just
    // its own text area). drawOnStrip() restores it.
    plotterview_refreshCursor();  // same MWCR0-clobber fix as on press
    pressedButton = Button::None;
}

// A finger on the Printer view's paper drags it (touch_set_drag_callback())
bool boardui_handleDrag(TouchDragPhase phase, std::uint16_t x, std::uint16_t y) {
    if (phase == TouchDragPhase::Begin) {
        if (demo_running() || backlight_dimmed()) return false;   // (that touch only stops / wakes)
        if (dialog_->isOpen() || infoBoxVisible || deviceListVisible) return false;
        if (x >= buttonStripScreenX0 && buttonStripVisible) return false;
        if (!plotterview_paperDragBegin(x, y)) return false;
        backlight_activity();
        return true;
    }
    if (phase == TouchDragPhase::Move) {
        plotterview_paperDragMove(x, y);
        backlight_activity();
    } else {
        plotterview_paperDragEnd();
        // Up to the tear-off edge and then sideways: tear it off? (asks)
        int dir = 0;
        if (plotterview_takeTearGesture(dir)) dialog_->openTearOff(dir);
    }
    return true;
}

void boardui_handleSwipe(bool forward) {
    if (demo_running()) { demo_stop(); return; }
    if (backlight_wakeByTouch()) return;
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
    // `forward` from touch.cpp means a left-to-right swipe. Swiping LEFT
    // goes to the next device on the loop (like turning a page), right
    // goes back.
    plotterview_cycleOutput(!forward);
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

// ── Screen saver ────────────────────────────────────────────────────────
// A drawn screen saver (clock, HP-IL rain, goose) gets the whole panel:
// the info box and device list close (left open behind the saver, the
// list could come back unseen yet still taking every touch); the menu and
// button strip go out of the way while it runs and come back as they were
// when it ends. Their own timers wait. They are PIP windows over the
// panel, so they are only switched off and on again
// (LT7683::setPipsBlanked()).

void boardui_closePipBoxes() {
    hideInfoBox();
    hideDeviceList();
}

void boardui_saverBegin() {
    if (saverActive) return;
    boardui_closePipBoxes();
    saverActive = true;
    display_->setPipsBlanked(true);
}

void boardui_saverEnd() {
    if (!saverActive) return;
    saverActive = false;
    display_->setPipsBlanked(false);
    // The timers start again, as if just used
    buttonStripHideDeadline = make_timeout_time_ms(kButtonStripHideMs);
    infoBoxHideDeadline = make_timeout_time_ms(kInfoBoxShowMs);
}

// ── Buttons from the PC (hipiview's keys, Mirror to PC < Control >) ─────
// Work like pressing the strip's buttons, except that the strip doesn't
// need to be showing first: it slides in (so the press can be seen) and
// the key acts at once. A held key is repeated by the PC; without news for
// kRemoteButtonTimeoutMs it counts as released.
namespace {
void releaseRemoteButton() {
    if (remoteButton == Button::None) return;
    remoteButton = Button::None;
    boardui_handleRelease();
}
}  // namespace

void boardui_remoteButton(std::uint8_t code, bool down) {
    if (down && demo_running()) { demo_stop(); return; }
    // 0: let go of everything (the session ended)
    if (code == 0) { releaseRemoteButton(); return; }
    // 6/7: next/previous view -- like a swipe to the left/right
    if (code == 6 || code == 7) {
        if (down) boardui_handleSwipe(/*forward (left-to-right)=*/code == 7);
        return;
    }
    if (code > 5) return;
    const Button b = static_cast<Button>(code);      // 1 Shift .. 5 X (ui_buttons.hpp)
    if (!down) {
        if (remoteButton == b) releaseRemoteButton();
        return;
    }
    if (remoteButton == b) {                          // still held: the PC repeats it
        remoteButtonHeard = get_absolute_time();
        return;
    }
    releaseRemoteButton();                            // another one: let go of the first
    if (pressedButton != Button::None) return;        // a finger is on a button
    remoteButtonHeard = get_absolute_time();
    if (backlight_wakeByTouch()) return;              // only wakes the screen, like a touch
    if (!buttonStripVisible) showButtonStrip();
    buttonStripHideDeadline = make_timeout_time_ms(kButtonStripHideMs);
    remoteButton = b;
    pressStripButton(b);
}

std::uint16_t boardui_buttonStripWidth() { return buttonStripWidth; }

bool boardui_isMenuOpen() {
    return dialog_ != nullptr && dialog_->isOpen();
}

void boardui_handleVerticalSwipe(bool down) {
    if (demo_running()) { demo_stop(); return; }
    if (backlight_wakeByTouch()) return;
    if (bTrace) {
        mLOGF("TOUCH", "vertical swipe down=%d deviceListVisible=%d", down, deviceListVisible);
    }
    if (deviceListVisible) {
        scrollDeviceList(down);
    }
    // No other use for a vertical swipe currently.
}

}  // namespace hipi