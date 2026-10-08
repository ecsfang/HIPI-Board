#include "glyph_font.h"
#include <algorithm>
#include <cstring>
#include <vector>

namespace hipi {

const GlyphFont::Glyph* GlyphFont::find(char c) const {
    const std::uint16_t code = static_cast<unsigned char>(c);
    int lo = 0, hi = count - 1;
    while (lo <= hi) {                       // glyphs are sorted by code
        const int mid = (lo + hi) / 2;
        if (glyphs[mid].code == code) return &glyphs[mid];
        if (glyphs[mid].code < code) lo = mid + 1;
        else                         hi = mid - 1;
    }
    return nullptr;
}

int GlyphFont::textWidth(const char* s) const {
    int w = 0;
    for (; *s; ++s) {
        if (const Glyph* g = find(*s)) w += g->advance;
    }
    return w;
}

void GlyphFont::textExtent(const char* s, int& minX, int& maxX) const {
    int pen = 0;
    minX = 0;
    maxX = 0;
    for (; *s; ++s) {
        const Glyph* g = find(*s);
        if (g == nullptr) continue;
        const int gx = pen + g->left;
        minX = std::min(minX, gx);
        maxX = std::max(maxX, gx + static_cast<int>(g->width));
        pen += g->advance;
    }
    maxX = std::max(maxX, pen);
}

namespace {
// RGB565 colour between bg (a = 0) and fg (a = 15)
std::uint16_t blend(std::uint16_t bg, std::uint16_t fg, int a) {
    const int rb = (bg >> 11) & 31, gb = (bg >> 5) & 63, bb = bg & 31;
    const int rf = (fg >> 11) & 31, gf = (fg >> 5) & 63, bf = fg & 31;
    const int r = rb + (rf - rb) * a / 15;
    const int g = gb + (gf - gb) * a / 15;
    const int b = bb + (bf - bb) * a / 15;
    return static_cast<std::uint16_t>((r << 11) | (g << 5) | b);
}
}  // namespace

int drawGlyphText(DisplayDriver* d, int x, int y, const GlyphFont& font, const char* s,
                  std::uint16_t fg, std::uint16_t bg) {
    // Where each glyph's bitmap goes, and the extent of the whole text
    struct Placed { const GlyphFont::Glyph* g; int x; };
    std::vector<Placed> placed;
    int pen = 0, minX = 0, maxX = 0;
    for (; *s; ++s) {
        const GlyphFont::Glyph* g = font.find(*s);
        if (g == nullptr) continue;
        const int gx = pen + g->left;
        placed.push_back(Placed{ g, gx });
        minX = std::min(minX, gx);
        maxX = std::max(maxX, gx + static_cast<int>(g->width));
        pen += g->advance;
    }
    maxX = std::max(maxX, pen);
    const int w = maxX - minX;
    if (w <= 0) return pen;

    std::uint16_t palette[16];
    for (int a = 0; a < 16; ++a) palette[a] = blend(bg, fg, a);
    // One row at a time over the whole text: coverage of overlapping
    // glyphs combined (max), then drawn in one go
    std::vector<std::uint8_t> cover(static_cast<std::size_t>(w));
    std::vector<std::uint16_t> row(static_cast<std::size_t>(w));
    for (int r = 0; r < font.height; ++r) {
        std::fill(cover.begin(), cover.end(), 0);
        for (const Placed& p : placed) {
            const int gw = p.g->width;
            const std::uint8_t* src = font.data + p.g->offset + r * ((gw + 1) / 2);
            for (int i = 0; i < gw; ++i) {
                const std::uint8_t b = src[i / 2];
                const std::uint8_t a = (i & 1) ? (b & 0x0F) : (b >> 4);
                std::uint8_t& c = cover[static_cast<std::size_t>(p.x - minX + i)];
                if (a > c) c = a;
            }
        }
        for (int i = 0; i < w; ++i) row[static_cast<std::size_t>(i)] = palette[cover[static_cast<std::size_t>(i)]];
        d->drawBitmap565(static_cast<std::int16_t>(x + minX), static_cast<std::int16_t>(y + r),
                         static_cast<std::uint16_t>(w), 1, row.data());
    }
    return pen;
}

void drawGlyphChar(DisplayDriver* d, int x, int y, const GlyphFont& font, char c,
                   std::uint16_t fg, std::uint16_t bg) {
    const GlyphFont::Glyph* g = font.find(c);
    if (g == nullptr || g->advance == 0) return;
    const int w = g->advance;
    std::uint16_t palette[16];
    for (int a = 0; a < 16; ++a) palette[a] = blend(bg, fg, a);
    std::vector<std::uint16_t> box(static_cast<std::size_t>(w) * font.height, bg);
    const int gw = g->width;
    for (int r = 0; r < font.height; ++r) {
        const std::uint8_t* src = font.data + g->offset + r * ((gw + 1) / 2);
        for (int i = 0; i < gw; ++i) {
            const int bx = g->left + i;
            if (bx < 0 || bx >= w) continue;
            const std::uint8_t b = src[i / 2];
            box[static_cast<std::size_t>(r * w + bx)] = palette[(i & 1) ? (b & 0x0F) : (b >> 4)];
        }
    }
    d->drawBitmap565(static_cast<std::int16_t>(x), static_cast<std::int16_t>(y),
                     static_cast<std::uint16_t>(w), font.height, box.data());
}

}  // namespace hipi
