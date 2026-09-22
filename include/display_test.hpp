// src/display_test.hpp
//
// Panel-agnostic hardware diagnostic tests -- works with whichever
// DisplayDriver this build targets (RA8875 on 5", LT7683 on 7"), via
// the common API surface both classes implement. Deliberately NEVER
// wired into the normal boot path (see pico_main.cpp, which doesn't
// include this header at all) -- call runDisplayTests(display) yourself,
// temporarily, from wherever's convenient when testing hardware/driver
// changes in isolation, without touching (and risking breaking) the
// rest of the running system.
#pragma once

#include "display_config.h"
#include "Screen.hpp"
#include "hp82163_font.hpp"
#include "pico/stdlib.h"
#include <cstdio>
#include "usb_serial.h"

namespace hipi {

inline void runDisplayTests(DisplayDriver* display) {
    LOGF("\r\n\r\n=== Display Diagnostic Tests ===\r\n");

    // ---- TEST 1: status register ----
    LOGF("\r\nTEST 1: Reading status register\r\n");
    std::uint8_t status = display->readStatus();
    LOGF("  Status register: 0x%02X\r\n", status);
    if (status == 0x00 || status == 0xFF) {
        LOGF("  FAIL: status looks wrong (no chip or short circuit) --"
             " check SPI wiring/CS/power\r\n");
    } else {
        LOGF("  OK: chip responding\r\n");
    }

    // ---- TEST 2: hardware reset ----
    LOGF("\r\nTEST 2: Hardware reset\r\n");
    display->reset();
    sleep_ms(100);
    status = display->readStatus();
    LOGF("  Status after reset: 0x%02X\r\n", status);

    // ---- TEST 3: text mode, cursor, single character (builtin/CGROM
    // font -- doesn't depend on anything CGRAM-related working) ----
    LOGF("\r\nTEST 3: Text mode, cursor, single character (builtin font)\r\n");
    display->selectBuiltinFont();
    display->txtSize(1);
    display->txtColor(0xFFFF, 0x0000);
    display->txtSetCursor(0, 0);
    display->txtWriteChar('A');
    LOGF("  Wrote 'A' at (0,0) in the chip's own builtin font --"
         " check if visible on screen\r\n");

    // ---- TEST 4: string write ----
    LOGF("\r\nTEST 4: Write string \"HELLO\"\r\n");
    display->txtSetCursor(0, 40);
    display->txtWrite("HELLO");
    LOGF("  Wrote \"HELLO\" at (0,40)\r\n");

    // ---- TEST 5: Screen class (the actual HP-41-emulation text path) ----
    LOGF("\r\nTEST 5: Screen class (pr_char loop)\r\n");
    Screen screen(display, 0xFFFF, 1, 200, SCREEN_MAX_X);
    const char* helloWorld = "HELLO WORLD!";
    for (const char* p = helloWorld; *p; ++p) screen.pr_char(*p);
    LOGF("  Wrote \"%s\" via Screen::pr_char()\r\n", helloWorld);

    // ---- TEST 6: CGRAM upload + read-back verification ----
    // Uploads a handful of representative characters fresh (not relying
    // on begin()'s own earlier upload still being valid) and immediately
    // reads each one back via the exact same Canvas/CVSSA/MRWDP
    // mechanism, symmetrically. If this reports MATCHES for all three,
    // the upload/readback mechanism itself is confirmed working at the
    // SDRAM level; a "custom font shows nothing on screen" issue would
    // then have to be in how the text-display side (selectCustomFont(),
    // CCR0 bit[7:6]=10b on LT7683 / FNCR0 bit7=1 on RA8875) looks the
    // data up, not in the upload itself. If any of these DON'T match,
    // the problem is confirmed to still be in the upload/readback
    // mechanism -- the read-back bytes themselves are logged too, to
    // help spot a pattern (e.g. all zero, all 0xFF, shifted by one,
    // etc.) rather than just a bare MATCH/no-match.
    LOGF("\r\nTEST 6: CGRAM upload + read-back verification\r\n");
    struct Check { std::uint8_t code; const std::uint8_t* bitmap; const char* label; };
    const Check checks[] = {
        { static_cast<std::uint8_t>('A'), font['A' - FONT_FIRST_ASCII], "'A'" },
        { static_cast<std::uint8_t>('0'), font['0' - FONT_FIRST_ASCII], "'0'" },
        { extra_font[0].code, extra_font[0].bitmap, "extra_font[0] (Swedish, \xC3\x84)" },
    };
    bool allMatched = true;
    for (const auto& c : checks) {
        display->uploadCgramChar(c.code, c.bitmap);
        std::uint8_t got[16] = {0};
        const bool ok = display->verifyCgramChar(c.code, c.bitmap, got);
        if (!ok) allMatched = false;
        LOGF("  code=0x%02X (%s): read-back %s\r\n", c.code, c.label,
             ok ? "MATCHES what was uploaded" : "DOES NOT MATCH -- upload/readback still broken");
        if (!ok) {
            LOGF("    expected:");
            for (int i = 0; i < 16; ++i) LOGF(" %02X", c.bitmap[i]);
            LOGF("\r\n    got:     ");
            for (int i = 0; i < 16; ++i) LOGF(" %02X", got[i]);
            LOGF("\r\n");
        }
    }

    // ---- TEST 7: custom font actually visible on screen ----
    // Only meaningful once TEST 6 above reports MATCHES -- otherwise
    // this is just confirming the same already-known-broken upload
    // shows nothing, which doesn't add new information.
    LOGF("\r\nTEST 7: Show custom-font 'A' on screen\r\n");
    if (!allMatched) {
        LOGF("  SKIPPED -- TEST 6 above didn't fully match, so this "
             "would just show blank/garbage for a reason we already know\r\n");
    } else {
        display->selectCustomFont();
        display->txtSize(1);
        display->txtSetCursor(0, 240);
        display->txtWriteChar('A');
        LOGF("  Wrote custom-font 'A' at (0,240) -- check whether an "
             "actual glyph is visible there (not blank)\r\n");
        display->selectBuiltinFont();  // leave the chip in the normal state
    }

    LOGF("\r\n=== Tests complete ===\r\n\r\n");
}

}  // namespace hipi
