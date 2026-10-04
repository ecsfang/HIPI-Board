// glyph_font.h -- pre-rendered, anti-aliased fonts (generated from a
// TrueType font by scripts/ttf_to_glyphs.py) and drawing them.
#pragma once
#include "display_config.h"
#include <cstdint>

namespace hipi {

struct GlyphFont {
    struct Glyph {
        std::uint16_t code;      // character
        std::uint16_t advance;   // pen movement, px (0 for the HP-41 font's ':' '.' ',')
        std::int16_t  left;      // bitmap start relative to the pen (<= 0)
        std::uint16_t width;     // bitmap width, px
        std::uint32_t offset;    // into data
    };
    std::uint16_t height;        // all glyphs, px
    std::uint16_t count;
    const Glyph* glyphs;         // sorted by code
    const std::uint8_t* data;    // 4-bit coverage, 2 px per byte, row by row

    const Glyph* find(char c) const;
    int textWidth(const char* s) const;
};

// Draws `s` with the pen starting at (x, y) (top of the glyphs), blending fg
// over bg by each pixel's coverage; overlapping glyphs (marks drawn into the
// previous character's space) are combined. Unknown characters are
// skipped. Returns the advance (the width textWidth() reports).
int drawGlyphText(DisplayDriver* d, int x, int y, const GlyphFont& font, const char* s,
                  std::uint16_t fg, std::uint16_t bg);

// One character in its own box (advance x font height, bg around it) in a
// single draw call -- faster for many small, separate characters (the
// rain screen saver). Glyphs reaching outside their box are clipped to it.
void drawGlyphChar(DisplayDriver* d, int x, int y, const GlyphFont& font, char c,
                   std::uint16_t fg, std::uint16_t bg);

}  // namespace hipi
