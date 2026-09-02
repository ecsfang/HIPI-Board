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
