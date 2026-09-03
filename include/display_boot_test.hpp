// SPDX-License-Identifier: MIT
//
// Minimal, panel-agnostic display bring-up/sanity-check test. Works
// against whichever DisplayDriver is selected (RA8875 or LT7683 -- see
// display_config.h), using only the shared public interface both
// implement, so the same test runs unmodified for either panel.
//
// Meant to be called once USB CDC logging is actually up (after the CDC
// connect-wait loops in main()) but still before anything else that
// depends on the SD card succeeding -- so it's fully usable while
// bringing up a board with no SD card connected yet, unlike the rest of
// the normal boot sequence. Calling it any earlier (e.g. right after
// initDisplay()) still drives the display correctly, but every LOGF()
// call inside it is silently dropped since nothing is listening on the
// USB CDC port yet -- see pico_main.cpp's call site for exactly where
// this needs to sit and why.
//
// Enabled via TEST_DISPLAY in pico_main.cpp -- off by default; only
// worth turning on again when bringing up a new/different board.
//
// NOTE on BTE: still unverified/likely broken on this board -- see
// LT7683::BTE()'s own header comment for the full bring-up story (S1
// variants, ROP codes, trigger/wait mechanics, active window, Main
// Image/Canvas addressing, Display Off/On cycling all checked out
// correct, yet the panel still ends up showing solid colour that
// survives a Display Off/On toggle -- needs real hardware inspection,
// e.g. an oscilloscope on SDRAM signals during a BTE op, to make further
// progress). LT7683::clearActiveWindow() used to go through BTE (a
// Solid Fill) and has been fixed to use the geometry engine instead,
// since Screen::clear()/full() call it on every construction and
// scroll -- that was freezing the display before any text could even
// be drawn. Two BTE call sites remain, not yet converted:
//   - boardui.cpp's showButtonStrip()/hideButtonStrip() (the button
//     strip slide animation) -- runNoBteSlideTest() below demonstrates
//     the same visual result using only fillRect(), as a template for
//     converting these too.
//   - Screen::bte() (Screen.cpp) -- used by pr_char()'s handling of
//     ESC J/K (clear to EOL/EOS), ESC L/M (insert/delete line). Not hit
//     by plain character printing, so not blocking basic text output,
//     but will need the same treatment before those editing features
//     work on this chip.
#pragma once

#include "display_config.h"
#include "boardui.h"
#include "usb_serial.h"
#include "pico/stdlib.h"
#include "tusb.h"
#include "touch.h"
#include <algorithm>

namespace hipi {

// sleep_ms() alone never services the USB stack -- fine for short delays,
// but this test's delays add up to several seconds total, and none of
// that time was feeding tud_task(). The CDC TX side can only drain up to
// a limited internal buffer without it; enough LOGF() output backing up
// during an unserviced multi-second stretch can overflow/corrupt that
// buffer. Breaks any delay into small slices with tud_task() pumped in
// between, so USB stays serviced throughout instead of only between
// whole LOGF()/sleep_ms() calls.
inline void sleepMsPumped(std::uint32_t ms) {
    constexpr std::uint32_t kSliceMs = 5;
    while (ms > kSliceMs) {
        tud_task();
        sleep_ms(kSliceMs);
        ms -= kSliceMs;
    }
    tud_task();
    sleep_ms(ms);
}

// Converts 8-bit R/G/B (0-255 each) to RGB565.
inline std::uint16_t rgb565(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    return static_cast<std::uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// Mirrors the core of ToSStudio/LT7683's own LT7683_Demo.ino -- a broad,
// known-good reference sketch explicitly built to "verify wiring and lib
// initialized correctly" for this exact chip family. Deliberately keeps
// to the parts backed by primitives this project already implements
// (line/rect/fillRect/circle/fillCircle/triangle/fillTriangle/
// roundRect/fillRoundRect/text) -- skips the demo's own compass/7-seg/
// BMW-spinner animation code, which isn't about verifying the driver.
inline void runReferenceDemoTest(DisplayDriver* display) {
    LOGF("Testing against ToSStudio/LT7683's own reference demo layout ...\r\n");

    constexpr std::uint16_t PW = 260, PH = 180, PX = 40, PY = 40;

    display->fill(rgb565(8, 18, 40));  // dark blue background, matches the demo's tft.back()

    // Outer border
    display->roundRect(0, 0, static_cast<std::int16_t>(display->width()),
                       static_cast<std::int16_t>(display->height()), 6, rgb565(220, 200, 80));

    // Primitives panel frame + title
    display->roundRect(PX, PY, PW, PH, 10, rgb565(120, 140, 180));
    display->txtColor(rgb565(200, 220, 255), rgb565(8, 18, 40));
    display->txtSize(0);
    display->txtSetCursor(PX + 10, PY + 6);
    display->txtWrite("PRIMITIVES");

    // The primitives themselves, same relative layout as drawPrimitivesDemo()
    const std::uint16_t shapeColor = rgb565(255, 200, 120);
    display->line(PX + 20, PY + 40, PX + 100, PY + 40, shapeColor);
    display->rect(PX + 20, PY + 60, 40, 40, shapeColor);
    display->fillRect(PX + 70, PY + 60, 40, 40, shapeColor);
    display->circle(PX + 150, PY + 70, 15, shapeColor);
    display->fillCircle(PX + 200, PY + 70, 15, shapeColor);
    display->triangle(PX + 30, PY + 130, PX + 10, PY + 160, PX + 50, PY + 160, shapeColor);
    display->fillTriangle(PX + 90, PY + 130, PX + 70, PY + 160, PX + 110, PY + 160, shapeColor);
    display->roundRect(PX + 130, PY + 120, 50, 40, 8, shapeColor);
    display->fillRoundRect(PX + 190, PY + 120, 50, 40, 8, shapeColor);

    // Text sizes/colours, same spirit as drawTextDemo()
    constexpr std::uint16_t TX = 40, TY = 260;
    display->roundRect(TX, TY, 260, 140, 10, rgb565(120, 140, 180));
    display->txtColor(rgb565(200, 220, 255), rgb565(8, 18, 40));
    display->txtSize(2);
    display->txtSetCursor(TX + 10, TY + 6);
    display->txtWrite("TEXT");

    display->txtColor(rgb565(255, 220, 120), rgb565(8, 18, 40));
    display->txtSize(0);
    display->txtSetCursor(TX + 12, TY + 50);
    display->txtWrite("Size 1");

    display->txtColor(rgb565(0, 255, 0), rgb565(8, 18, 40));
    display->txtSize(1);
    display->txtSetCursor(TX + 12, TY + 74);
    display->txtWrite("Size 2");

    display->txtColor(rgb565(255, 0, 0), rgb565(8, 18, 40));
    display->txtSize(0);
    display->txtSetCursor(TX + 12, TY + 108);
    display->txtWrite("Scale x1");

    LOGF("done\r\n");
    sleepMsPumped(2000);
}

// Slide-in/slide-out WITHOUT BTE (see the file header for why) -- redraws
// the moving square FRESH at its new position every frame, clearing the
// lane it travels through first. No existing on-screen pixels are ever
// preserved/shifted -- just erase-and-redraw, which is all a
// *solid-colour* box like this needs. For the real button-strip bitmap
// case, the same idea applies with drawBitmap565Cropped() cropping fresh
// from the source data in MCU memory each frame instead of a flat
// fillRect() colour.
inline void runNoBteSlideTest(DisplayDriver* display) {
    LOGF("Testing slide-in/slide-out without BTE (clear+redraw each frame) ...\r\n");

    constexpr std::int16_t laneY = 50, laneH = 80;
    constexpr std::int16_t boxW = 80, boxH = 80;
    constexpr std::int16_t finalX = 350;
    constexpr std::uint16_t redColor = 0xF800;
    constexpr std::uint16_t greenColor = 0x07E0;
    constexpr int steps = 20;
    constexpr std::uint32_t stepDelayMs = 60;

    const std::int16_t panelW = static_cast<std::int16_t>(display->width());
    const std::int16_t startX = panelW;  // fully off-screen to the right

    display->fill(0x0000);
    display->fillRect(50, 50, boxW, boxH, redColor);  // fixed reference square
    sleepMsPumped(500);

    for (int i = 0; i <= steps; ++i) {
        const std::int16_t x = static_cast<std::int16_t>(
            startX - ((startX - finalX) * i) / steps);
        display->fillRect(130, laneY, static_cast<std::int16_t>(panelW - 130), laneH, 0x0000);
        const std::int16_t visW = static_cast<std::int16_t>(
            (x + boxW <= panelW) ? boxW : (panelW - x));
        if (x < panelW && visW > 0) display->fillRect(x, laneY, visW, boxH, greenColor);
        sleepMsPumped(stepDelayMs);
    }
    sleepMsPumped(1500);

    for (int i = 0; i <= steps; ++i) {
        const std::int16_t x = static_cast<std::int16_t>(
            finalX + ((startX - finalX) * i) / steps);
        display->fillRect(130, laneY, static_cast<std::int16_t>(panelW - 130), laneH, 0x0000);
        const std::int16_t visW = static_cast<std::int16_t>(
            (x + boxW <= panelW) ? boxW : (panelW - x));
        if (x < panelW && visW > 0) display->fillRect(x, laneY, visW, boxH, greenColor);
        sleepMsPumped(stepDelayMs);
    }
    LOGF("done\r\n");
    sleepMsPumped(500);
}

// Visual touch calibration -- draws corner/edge reference labels with
// their true coordinates, plus a fixed centre crosshair, then tracks
// whatever's touched in real time: logs the raw (x,y) every time it
// changes, and draws a small crosshair right at that position (erasing
// the previous one first). Lets you immediately SEE whether touch
// coordinates land where your finger actually is -- e.g. touching the
// top-left corner should draw a marker right next to the "TOP-LEFT"
// label, not somewhere else entirely (a common first-bring-up issue:
// X/Y swapped, or one axis inverted, relative to the panel's own mounted
// orientation -- something datasheets don't usually specify and has to
// be confirmed empirically per board). If that happens, the fix goes in
// touch_get_point() itself (see its own comment in touch.cpp) -- nothing
// about the debounce/tap logic in touch_poll() needs to change for it.
//
// Runs for runMs (default 60s) -- polls touch_get_point() directly
// rather than going through touch_poll()'s tap/swipe debounce, since
// what's being verified here is the raw coordinate mapping itself, not
// the gesture logic built on top of it (that's already covered by
// boardui.cpp's own tap/release/swipe callbacks in normal operation).
// Cycles through all 4 combinations of VDIR (REG[12h] bit3) and MACR
// (REG[02h] bit[2:1]) live, each held for several seconds with a clearly
// labelled, asymmetric layout (corner labels + an arrow-like shape) --
// settling by direct observation which combination is actually correct,
// rather than reasoning from the datasheet's own worked examples: two
// different attempts based on that reasoning (VDIR alone, and VDIR+MACR
// together) both produced the same real-hardware result (correct
// vertical orientation, but mirrored text), which shouldn't be possible
// if MACR's bit[2:1] were doing what Section 5.4 describes -- something
// about this board's actual behaviour isn't matching that description,
// so this settles it empirically instead of guessing a third time.
inline void runOrientationTest(DisplayDriver* display) {
    LOGF("Testing display orientation: cycling all 4 VDIR/MACR combos ...\r\n");

    struct Combo {
        const char* name;
        std::uint8_t macr;   // REG[02h]
        std::uint8_t dpcr;   // REG[12h]
    };
    const Combo combos[] = {
        {"1: VDIR=0 MACR=00 (normal/original)",           0x40, 0x80},
        {"2: VDIR=0 MACR=01 (horizontal flip only)",      0x42, 0x80},
        {"3: VDIR=1 MACR=00 (vertical flip only)",        0x40, 0x88},
        {"4: VDIR=1 MACR=01 (both -- datasheet's '180')",  0x42, 0x88},
    };

    const std::int16_t w = static_cast<std::int16_t>(display->width());
    const std::int16_t h = static_cast<std::int16_t>(display->height());

    for (const auto& c : combos) {
        LOGF("  Combo %s ...\r\n", c.name);
        display->writeReg(0x02, c.macr);
        display->writeReg(0x12, c.dpcr);
        // Re-assert the active window after changing MACR/VDIR live --
        // changing memory store/scan direction mid-operation, without
        // this, produced a black screen with no visible content at all
        // on real hardware (confirmed), even though the code kept
        // running fine (log lines still appeared in order). begin()
        // itself never needs this since it sets MACR/VDIR once as part
        // of one careful, ordered startup sequence -- doing the same
        // change live, later, seems to need the addressing explicitly
        // re-synced afterward.
        display->setActiveWindow(0, 0, static_cast<std::uint16_t>(w - 1),
                                 static_cast<std::uint16_t>(h - 1));

        display->fill(0x0000);
        display->txtColor(0xFFFF, 0x0000);
        display->txtSize(1);
        display->txtSetCursor(20, 20);
        display->txtWrite(c.name);
        display->txtSetCursor(20, 60);
        display->txtWrite("ABCDEFGHIJ 1234567890");
        // Asymmetric corner labels + a right-pointing arrow shape --
        // deliberately NOT symmetric in either axis, so every one of the
        // 4 combos looks visually distinct, not just "flipped text".
        display->txtSetCursor(5, 5);
        display->txtWrite("TL");
        display->txtSetCursor(static_cast<std::uint16_t>(w - 30), 5);
        display->txtWrite("TR");
        display->txtSetCursor(5, static_cast<std::uint16_t>(h - 20));
        display->txtWrite("BL");
        display->txtSetCursor(static_cast<std::uint16_t>(w - 30), static_cast<std::uint16_t>(h - 20));
        display->txtWrite("BR");
        const std::int16_t ax = static_cast<std::int16_t>(w / 2 - 40);
        const std::int16_t ay = static_cast<std::int16_t>(h / 2);
        display->line(ax, ay, static_cast<std::int16_t>(ax + 80), ay, 0x07E0);
        display->line(static_cast<std::int16_t>(ax + 60), static_cast<std::int16_t>(ay - 20),
                      static_cast<std::int16_t>(ax + 80), ay, 0x07E0);
        display->line(static_cast<std::int16_t>(ax + 60), static_cast<std::int16_t>(ay + 20),
                      static_cast<std::int16_t>(ax + 80), ay, 0x07E0);

        sleepMsPumped(6000);
    }

    LOGF("Orientation test done. Which numbered combo (1-4) looked fully\r\n");
    LOGF("correct -- text upright and left-to-right, TL/TR/BL/BR labels in\r\n");
    LOGF("their true corners, arrow pointing right?\r\n");
}

inline void runTouchCalibrationTest(DisplayDriver* display, std::uint32_t runMs = 60000) {
    LOGF("Testing touch calibration -- tap anywhere and watch for a marker\r\n");
    LOGF("at the touched position. Raw (x,y) logged on every change.\r\n");
    LOGF("Running for %lus ...\r\n", static_cast<unsigned long>(runMs / 1000));

    // This test runs early in the boot sequence (before touchInit() would
    // normally be called much later on, after SD-card/display setup) --
    // self-contained here rather than depending on that ordering.
    touchInit();

    const std::int16_t w = static_cast<std::int16_t>(display->width());
    const std::int16_t h = static_cast<std::int16_t>(display->height());

    display->fill(0x0000);
    display->txtColor(0xFFFF, 0x0000);
    display->txtSize(0);

    char buf[32];
    display->txtSetCursor(5, 5);
    display->txtWrite("TOP-LEFT (0,0)");
    std::snprintf(buf, sizeof(buf), "TOP-RIGHT (%d,0)", w - 1);
    display->txtSetCursor(static_cast<std::uint16_t>(w - 150), 5);
    display->txtWrite(buf);
    std::snprintf(buf, sizeof(buf), "BOT-LEFT (0,%d)", h - 1);
    display->txtSetCursor(5, static_cast<std::uint16_t>(h - 20));
    display->txtWrite(buf);
    std::snprintf(buf, sizeof(buf), "BOT-RIGHT (%d,%d)", w - 1, h - 1);
    display->txtSetCursor(static_cast<std::uint16_t>(w - 160), static_cast<std::uint16_t>(h - 20));
    display->txtWrite(buf);

    // Fixed centre crosshair -- a stationary reference point, untouched
    // by the moving touch marker below (drawn far enough inset that the
    // two never overlap in practice).
    const std::int16_t cx = static_cast<std::int16_t>(w / 2);
    const std::int16_t cy = static_cast<std::int16_t>(h / 2);
    display->line(static_cast<std::int16_t>(cx - 15), cy, static_cast<std::int16_t>(cx + 15), cy, 0x7BEF);
    display->line(cx, static_cast<std::int16_t>(cy - 15), cx, static_cast<std::int16_t>(cy + 15), 0x7BEF);

    constexpr std::int16_t kMarkerHalf = 8;
    bool wasDown = false;
    std::int16_t lastX = 0, lastY = 0;

    const absolute_time_t deadline = make_timeout_time_ms(runMs);
    while (!time_reached(deadline)) {
        tud_task();

        std::uint16_t rx, ry;
        const bool down = touch_get_point(rx, ry);
        if (down) {
            const std::int16_t x = static_cast<std::int16_t>(rx);
            const std::int16_t y = static_cast<std::int16_t>(ry);
            if (!wasDown || x != lastX || y != lastY) {
                LOGF("  touch: (%d, %d)\r\n", x, y);
                if (wasDown) {
                    // Erase the previous marker's bounding box (clamped to
                    // stay on-screen so a marker near an edge doesn't try
                    // to erase/redraw off-panel).
                    const std::int16_t ex0 = std::max<std::int16_t>(0, static_cast<std::int16_t>(lastX - kMarkerHalf));
                    const std::int16_t ey0 = std::max<std::int16_t>(0, static_cast<std::int16_t>(lastY - kMarkerHalf));
                    const std::int16_t ex1 = std::min<std::int16_t>(w, static_cast<std::int16_t>(lastX + kMarkerHalf + 1));
                    const std::int16_t ey1 = std::min<std::int16_t>(h, static_cast<std::int16_t>(lastY + kMarkerHalf + 1));
                    display->fillRect(ex0, ey0, static_cast<std::int16_t>(ex1 - ex0),
                                      static_cast<std::int16_t>(ey1 - ey0), 0x0000);
                }
                const std::int16_t lx0 = std::max<std::int16_t>(0, static_cast<std::int16_t>(x - kMarkerHalf));
                const std::int16_t lx1 = std::min<std::int16_t>(static_cast<std::int16_t>(w - 1), static_cast<std::int16_t>(x + kMarkerHalf));
                const std::int16_t ly0 = std::max<std::int16_t>(0, static_cast<std::int16_t>(y - kMarkerHalf));
                const std::int16_t ly1 = std::min<std::int16_t>(static_cast<std::int16_t>(h - 1), static_cast<std::int16_t>(y + kMarkerHalf));
                display->line(lx0, y, lx1, y, 0xF800);
                display->line(x, ly0, x, ly1, 0xF800);
                lastX = x;
                lastY = y;
            }
            wasDown = true;
        } else {
            wasDown = false;
        }
        sleep_ms(20);
    }
    LOGF("Touch calibration test done.\r\n");
}

inline void runDisplayBootTest(DisplayDriver* display) {
    LOGF("\r\n\r\n=== Display boot test ===\r\n");

    // ---- Step 1: basic SPI/status sanity ----
    // A status byte that's stuck at 0x00 or 0xFF across two reads usually
    // means the chip isn't answering at all -- wrong SPI pins, no power,
    // or (for LT7683 specifically) still held in reset. A changing or
    // non-trivial value means SPI reads/writes are at least reaching the
    // chip and getting a real answer back.
    std::uint8_t s1 = display->readStatus();
    sleepMsPumped(5);
    std::uint8_t s2 = display->readStatus();
    LOGF("Status register: 0x%02X, 0x%02X (two reads)\r\n", s1, s2);
    if ((s1 == 0x00 || s1 == 0xFF) && s1 == s2) {
        LOGF("  WARNING: looks stuck -- check SPI wiring/power/reset before anything else\r\n");
    } else {
        LOGF("  OK: chip is responding over SPI\r\n");
    }

    // ---- Step 1a: orientation (VDIR/MACR combo) ----
    // Disabled -- live-cycling through VDIR/MACR combos this way produced
    // a black screen with no visible content on real hardware, even for
    // combinations confirmed to work fine when set once during begin()
    // instead. See LT7683::begin()'s own comment for the current state
    // of this investigation; each combination needs its own separate
    // rebuild-and-flash to test cleanly for now.
    // runOrientationTest(display);

    // ---- Step 1b: touch calibration ----
    runTouchCalibrationTest(display);

    // ---- Step 2: reference demo layout (primitives + text) ----
    runReferenceDemoTest(display);

    // ---- Step 3: full-screen solid colour fills ----
    // Deliberately the simplest possible operation (one hardware fill
    // covering the whole panel) -- if PCLK/HSYNC/VSYNC/backlight are all
    // working, each colour should be unmistakable from across the room,
    // regardless of what state text/CGRAM/bitmaps are in. If NOTHING
    // appears through all three, the problem is upstream of anything
    // this project's drawing code controls (panel timing, backlight
    // power, or the panel/cable itself), not in text/bitmap code.
    struct ColorStep { const char* name; std::uint16_t color; };
    const ColorStep colors[] = {
        {"RED",   0xF800},
        {"GREEN", 0x07E0},
        {"BLUE",  0x001F},
    };
    for (const auto& c : colors) {
        LOGF("Filling screen: %-5s ... ", c.name);
        display->fill(c.color);
        LOGF("done\r\n");
        sleepMsPumped(600);
    }

    // ---- Step 4: corner markers ----
    // A 20x20 square in each of the four true corners, plus one centred --
    // deliberately drawn from the driver's own width()/height() rather
    // than a literal panel size, so if the *wrong* panel size ever ends up
    // selected (or an active-window clip is left stuck at some other
    // size), the mismatch is immediately, visually obvious: any square
    // landing short of the physical edge, or missing entirely, means
    // something is clipping to less than the full panel.
    LOGF("Drawing corner markers (%ux%u) ... ", display->width(), display->height());
    display->fill(0x0000);
    const std::int16_t w = static_cast<std::int16_t>(display->width());
    const std::int16_t h = static_cast<std::int16_t>(display->height());
    constexpr std::int16_t m = 20;  // marker size
    display->fillRect(0, 0, m, m, 0xF800);                  // top-left: red
    display->fillRect(static_cast<std::int16_t>(w - m), 0, m, m, 0x07E0);              // top-right: green
    display->fillRect(0, static_cast<std::int16_t>(h - m), m, m, 0x001F);              // bottom-left: blue
    display->fillRect(static_cast<std::int16_t>(w - m), static_cast<std::int16_t>(h - m), m, m, 0xFFE0);  // bottom-right: yellow
    display->fillRect(static_cast<std::int16_t>((w - m) / 2), static_cast<std::int16_t>((h - m) / 2), m, m, 0xFFFF);  // centre: white
    LOGF("done\r\n");
    sleepMsPumped(1000);

    // ---- Step 5: text output ----
    LOGF("Drawing text test ... ");
    display->fill(0x0000);
    display->rect(0, 0, w, h, 0xFFFF);
    display->txtColor(0xFFFF, 0x0000);
    display->txtSize(1);
    display->txtSetCursor(20, 20);
    display->txtWrite("DISPLAY BOOT TEST OK");
    display->txtSize(0);
    display->txtSetCursor(20, 60);
    char buf[48];
    std::snprintf(buf, sizeof(buf), "Panel: %u x %u", display->width(), display->height());
    display->txtWrite(buf);
    display->txtColor(0x07E0, 0x0000);
    display->txtSetCursor(20, 80);
    display->txtWrite("abcdefghijklmnopqrstuvwxyz");
    display->txtSetCursor(20, 100);
    display->txtWrite("ABCDEFGHIJKLMNOPQRSTUVWXYZ 0123456789");
    display->txtColor(0xFFFF, 0x0000);
    display->txtSetCursor(static_cast<std::uint16_t>(w - 130),
                           static_cast<std::uint16_t>(h - 20));
    display->txtWrite("BOTTOM-RIGHT");
    LOGF("done\r\n");
    sleepMsPumped(1500);

    // ---- Step 6: splash screen ----
    // Exercises the actual production showSplashScreen() (same function
    // pico_main.cpp calls during a normal boot), not a simplified stand-in
    // -- so this also verifies the fix to its own hardcoded-800x480 bug
    // (see boardui.cpp). No SD card needed: logo.bmp is optional and
    // gracefully skipped if missing, falling back to text-only.
    LOGF("Showing splash screen ... ");
    showSplashScreen(display, "TEST", 2000);
    LOGF("done\r\n");

    // ---- Step 7: slide-in/slide-out (no BTE -- see file header) ----
    runNoBteSlideTest(display);

    LOGF("=== Display boot test complete ===\r\n\r\n");
}

}  // namespace hipi
