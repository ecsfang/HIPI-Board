// src/display_test.hpp
#pragma once

#include "RA8875.hpp"
#include "Screen.hpp"
#include "pico/stdlib.h"
#include <cstdio>
#include "usb_serial.h"

namespace hp82163 {

// Forward-declare: vi behöver en funktion vi kan anropa från main
inline void runDisplayTests(hp82163::RA8875* display) {
    LOGF("\n=== RA8875 Display Diagnostic Tests ===\n\n");

    // ---- TEST 1: Läs status-register ----
    LOGF("TEST 1: Reading status register\n");
    std::uint8_t status = display->readStatus();
    LOGF("  Status register: 0x%02X\n", status);
    if (status == 0x00 || status == 0xFF) {
        LOGF("  FAIL: Status looks wrong (no chip or short circuit)\n");
        LOGF("  Check: SPI wiring, CS pin, power\n");
    } else {
        LOGF("  OK: Chip responding\n");
    }
    LOGF("\n");

    // ---- TEST 2: Hård-reset ----
    LOGF("TEST 2: Hardware reset\n");
    display->reset();
    sleep_ms(100);
    status = display->readStatus();
    LOGF("  Status after reset: 0x%02X\n", status);
    LOGF("\n");

    // ---- TEST 3: Identifiera chip ----
    LOGF("TEST 3: Chip identification\n");
    std::uint8_t chip = display->readReg(0x00);
    LOGF("  Register 0x00: 0x%02X (should be 0x75)\n", chip);
    if (chip != 0x75) {
        LOGF("  FAIL: Not an RA8875!\n");
        return;
    }
    LOGF("  OK: RA8875 confirmed\n");
    LOGF("\n");

    // ---- TEST 4: System config ----
    LOGF("TEST 4: System configuration register\n");
    std::uint8_t sysr = display->readReg(0x10);  // SYSR
    LOGF("  SYSR (0x10): 0x%02X\n", sysr);
    LOGF("    8BPP: %s\n", (sysr & 0x0C) == 0x00 ? "YES" : "NO");
    LOGF("    16BPP: %s\n", (sysr & 0x0C) == 0x0C ? "YES" : "NO");
    LOGF("\n");

    // ---- TEST 5: Memory clear ----
    LOGF("TEST 5: Memory clear\n");
    display->writeReg(RA8875::MCLR, RA8875::MCLR_START | RA8875::MCLR_FULL);
    sleep_ms(500);
    LOGF("  Memory cleared (full screen)\n");
    LOGF("\n");

    // ---- TEST 6: Byt till text mode ----
    LOGF("TEST 6: Switch to text mode\n");
    display->txtMode();
    display->txtSize(0);  // size 0 = built-in 8x16
    display->txtColor(0xFF, 0x00);  // white on black
    LOGF("  Text mode active\n");
    LOGF("\n");

    // ---- TEST 7: Sätt cursor ----
    LOGF("TEST 7: Set text cursor\n");
    display->txtSetCursor(0, 0);
    LOGF("  Cursor at (0, 0)\n");
    LOGF("\n");

    // ---- TEST 8: Skriv ett tecken ----
    LOGF("TEST 8: Write single character 'A'\n");
    display->txtWriteChar('A');
    sleep_ms(100);
    LOGF("  Wrote 'A', check if visible\n");
    LOGF("\n");

    // ---- TEST 9: Skriv en sträng ----
    LOGF("TEST 9: Write string 'HELLO'\n");
    display->txtSetCursor(0, 16);  // next line
    display->txtWrite("HELLO");
    sleep_ms(100);
    LOGF("  Wrote 'HELLO'\n");
    LOGF("\n");

    // ---- TEST 10: Testa Screen-klassen ----
    LOGF("TEST 10: Test Screen class\n");
    // Clear the entire window
    display->writeReg(RA8875::MCLR, RA8875::MCLR_START | RA8875::MCLR_FULL);
    sleep_ms(500);
    LOGF("  Cleared\n");

// ---- TEST 11: Verify CGRAM was uploaded ----
LOGF("TEST 11: Verify CGRAM upload\n");

// Skriv 'A' med CGRAM (char code 33 i vår font)
// A är index 65-32 = 33 i vår CGRAM
display->writeReg(0x23, 65);     // CGRAM address 65 (ASCII 'A')
display->writeReg(0x21, 0x00);   // Clear FNCR0
display->writeCmd(RA8875::MRWC);
std::uint8_t fontByte = 0;
for (int i = 0; i < 16; i++) {
    display->writeData(fontByte);  // ← borde visa en kolumn av A
    LOGF("  CGRAM byte %d: 0x%02X\n", i, fontByte);
}
LOGF("\n");

// ---- TEST 12: Write text without setting cursor first ----
LOGF("TEST 12: Write 'X' without cursor set\n");

// Cursor blinkar på (0,0) — skriv dit
display->writeCmd(RA8875::MRWC);
display->writeData('X');  // Skriver till (0,0) — borde täcka cursor

LOGF("  Wrote 'X' at default cursor position\n");
LOGF("  If 'X' replaces cursor → text mode works\n");
LOGF("  If cursor still blinks → write didn't take effect\n");
LOGF("\n");

// ---- TEST 13: Read back CGRAM ----
LOGF("TEST 13: Read back CGRAM to verify\n");

display->writeReg(0x23, 65);  // CGRAM address för 'A'
display->writeCmd(RA8875::MRWC);
std::uint8_t first_byte = display->readData();
LOGF("  First byte of 'A' CGRAM: 0x%02X\n", first_byte);
// Förväntat: ~60-126 (binärt 0x60) för 'A' i vår fontomotstånd
LOGF("  Should be non-zero (font data)\n");
LOGF("\n");

// ---- TEST 14: Try 16BPP text mode ----
LOGF("TEST 14: 16BPP text mode\n");

display->writeReg(0x10, 0x0C);  // SYSR = 16BPP
display->txtMode();
display->txtSize(0);
display->txtColor(0xFFFF, 0x0000);  // white on black
display->txtSetCursor(0, 0);
display->txtWriteChar('A');
LOGF("  Wrote 'A' in 16BPP mode\n");
LOGF("\n");

// ---- TEST 15: Force-select layer 1 ----
LOGF("TEST 15: Select layer 1\n");

display->writeReg(0x52, 0x00);  // layer 1 active
display->writeReg(0x53, 0x00);  // layer 1 visible
sleep_ms(100);
LOGF("  Layer 1 should be visible\n");
LOGF("\n");

// ---- TEST 16: Upload CGRAM with corrected code ----
LOGF("TEST 16: Upload CGRAM\n");

// Manually upload 'A' (font index 33 in our 8x16 font)
const std::uint8_t A_bitmap[16] = {
    60, 102, 195, 195, 195, 195, 195, 195,
    195, 219, 207, 102, 60, 0, 0, 0
};

display->uploadCgramChar(65, A_bitmap);  // ASCII 'A' = 65
LOGF("  Uploaded 'A' to CGRAM\n");

// Verify
display->writeReg(0x23, 65);
display->writeCmd(RA8875::MRWC);
first_byte = display->readData();
LOGF("  First byte of 'A' CGRAM after upload: 0x%02X\n", first_byte);
LOGF("  Should be 60 (0x3C) for 'A'\n");
LOGF("\n");

// ---- TEST 17: Direct CGRAM write ----
LOGF("TEST 17: Direct CGRAM write\n");

display->writeReg(0x23, 65);          // CGRAM address för 'A'
display->writeReg(0x21, 0x00);        // FNCR0
display->writeReg(0x41, 0x04);        // MWCR0: CGRAM mode + font select
display->writeCmd(RA8875::MRWC);

const std::uint8_t test_data[16] = {
    60, 102, 195, 195, 195, 195, 195, 195,
    195, 219, 207, 102, 60, 0, 0, 0
};
for (int i = 0; i < 16; i++) {
    display->writeData(test_data[i]);
}

display->writeReg(0x41, 0x00);        // Återställ
LOGF("  Wrote directly\n");

// Verify
display->writeReg(0x23, 65);
display->writeCmd(RA8875::MRWC);
first_byte = display->readData();
LOGF("  First byte: 0x%02X\n", first_byte);
LOGF("\n");

    Screen screen(display, 0xFF, 0, 200);
    LOGF("  Screen created (color=0xFF, size=0, bright=200)\n");
    
    const char* test = "HELLO WORLD!";
    for (const char* p = test; *p; ++p) {
        screen.pr_char(*p);
    }
    LOGF("  Wrote '%s' via Screen.pr_char\n", test);
    LOGF("\n");
    
    LOGF("=== Tests complete ===\n");
    LOGF("Did you see 'A', 'HELLO', or 'HELLO WORLD!' on screen?\n");
}

}  // namespace hp82163
