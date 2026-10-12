#pragma once
// The HP 82162A's character generator (printer_font.cpp): [code][column],
// 5 columns per character, bit 0 = top dot, codes 0..127.
#include <cstdint>

extern const std::uint8_t kPrinterFontAscii[128][5];   // escape mode
extern const std::uint8_t kPrinterFontHp41[128][5];    // 8-bit mode (HP-41)
