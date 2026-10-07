#include "chartable.h"
#include "glyph_font.h"
#include <cstdio>
#include <string>

namespace hipi {

extern const GlyphFont kCharTableFont;     // src/chartable_font.cpp

namespace {
DisplayDriver* d_ = nullptr;
constexpr std::uint16_t kYellow = 0xEDC0, kBlack = 0x0000, kWhite = 0xFFFF;
constexpr std::uint16_t kGrey = 0x7BEF, kGrid = 0x2104;

void text(int x, int y, const char* s, std::uint16_t fg, std::uint16_t bg) {
    d_->txtSize(0);
    d_->txtColor(fg, bg);
    d_->txtSetCursor(static_cast<std::uint16_t>(x), static_cast<std::uint16_t>(y));
    d_->txtWrite(s);
}

void render() {
    d_->setActiveWindow(0, 0, SCREEN_MAX_X - 1, SCREEN_MAX_Y - 1);
    d_->selectBuiltinFont();
    d_->fillRect(0, 0, SCREEN_MAX_X, SCREEN_MAX_Y, kBlack);
    d_->fillRect(0, 0, SCREEN_MAX_X, 26, kYellow);
    text(8, 5, "HP-41 CHARACTER TABLE", kBlack, kYellow);
    text(240, 5, "the HP-41's character codes 0-127 (decimal) as its display shows them", kBlack, kYellow);

    // 8 rows of 16: code = row * 16 + column, 0..127 -- the HP-41's own
    // codes, characters as its display shows them (src/chartable_font.cpp
    // is indexed by HP-41 code; see scripts/hp41_charset.py)
    constexpr int kTop = 30, kCellW = SCREEN_MAX_X / 16, kCellH = (SCREEN_MAX_Y - kTop - 24) / 8;
    for (int i = 0; i <= 16; ++i)
        d_->fillRect(i * kCellW - (i == 16 ? 1 : 0), kTop, 1, kCellH * 8, kGrid);
    for (int i = 0; i <= 8; ++i)
        d_->fillRect(0, kTop + i * kCellH, SCREEN_MAX_X, 1, kGrid);
    char num[8];
    for (int code = 0; code < 128; ++code) {
        const int x = (code % 16) * kCellW, y = kTop + (code / 16) * kCellH;
        std::snprintf(num, sizeof(num), "%d", code);
        text(x + 4, y + 4, num, kGrey, kBlack);
        const GlyphFont::Glyph* g = kCharTableFont.find(static_cast<char>(code));
        if (g == nullptr) continue;
        // The character in the cell's lower right part (one glyph = one box)
        const int gx = x + kCellW - 6 - g->width;
        const int gy = y + kCellH - 6 - kCharTableFont.height;
        drawGlyphChar(d_, gx, gy, kCharTableFont, static_cast<char>(code), kWhite, kBlack);
    }
    text(8, SCREEN_MAX_Y - 20, "Touch the screen to close.", kGrey, kBlack);
}
}  // namespace

void chartable_init(DisplayDriver* display) { d_ = display; }

void chartable_drawAll() {
    if (d_ == nullptr) return;
#ifdef DISPLAY_7INCH
    // Off-screen, then copied in one go (no flicker), like the loop map
    const std::uint32_t prevAddr = d_->canvasAddr();
    const std::uint16_t prevStride = d_->canvasStride();
    d_->beginLayerDraw(LT7683::kBteLayer3Addr);
    render();
    if (prevAddr == 0) d_->endOverlayDraw();
    else               d_->beginLayerDraw(prevAddr, prevStride);
    d_->copyLayerToPanel(LT7683::kBteLayer3Addr, 0, 0, SCREEN_MAX_X, SCREEN_MAX_Y);
#else
    render();
#endif
}

}  // namespace hipi
