// menu_frame.h -- the look of every menu and dialog: a black box with a
// thick, rounded yellow frame and the title in a yellow band at the top,
// written in the HP-41's display font (or the built-in font, see
// HIPI_MENU_TITLE_HP41FONT). Shared by UiDialog (uidialog.hpp), the boxes
// in boardui.cpp and the Create media dialogs (media_create_ui.h).
#pragma once
#include "display_config.h"
#include "glyph_font.h"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>

// Menu titles in the HP-41 display font (1) or the built-in font (0)
#ifndef HIPI_MENU_TITLE_HP41FONT
#define HIPI_MENU_TITLE_HP41FONT 1
#endif

namespace hipi {

extern const GlyphFont kMenuTitleFont;   // src/menu_font_title.cpp

// Shared box position/size/style for the on-screen menu frame. Pulled out
// to namespace scope so the splash screen (shown before any UiDialog
// exists) can draw an identical-looking frame without duplicating the
// yellow/radius/thickness constants.
namespace MenuFrame {
    // W widened to fit filenames up to ~32 characters at TextScale (16px/char
    // at scale 1) plus margins, while still staying clear of the button
    // strip at x=680.
    constexpr int X = 60, Y = 80, W = 580, H = 260;
    constexpr int BorderThickness = 3;
    constexpr int CornerRadius    = 14;

    // Same yellow/orange as used in buttons.bmp (#E8B800 -> RGB565).
    // Now that the display runs in genuine 16bpp mode, this renders as the
    // real, correct color -- no more of the 8bpp "green mod 8" quirk we
    // had to work around before (see git history / prior conversation).
    constexpr std::uint16_t Yellow = 0xEDC0;

    // The display chip only has one hardware text-scale register, shared by
    // Screen (HP-41 video emulation) and the menu. Screen can change it
    // at any time (font-size menu, or an incoming ESC sequence), so the
    // menu always forces it back to this fixed scale before drawing --
    // otherwise menu rows would grow/shrink along with the HP-41 font.
    constexpr std::uint8_t TextScale = 1;

    // Row pitch must fit TextScale's glyph height (16px base * (1+scale)
    // = 32px at scale 1) with a bit of breathing room.
    constexpr int RowPitch = 36;

    // The menus' title band, above the rows: the frame starts TitleH
    // higher than Y (rows still start at Y + 20)
    constexpr int TitleH = 30;
    constexpr int MenuTop = Y - TitleH;
    constexpr int MenuH = H + TitleH;

    // Thick, rounded frame: fill the whole box yellow (rounded corners),
    // then lay a smaller rounded black rectangle on top, inset by
    // BorderThickness on each side -> leaves an even yellow border.
    inline void draw(DisplayDriver* d, int x = X, int y = Y, int w = W, int h = H) {
        d->txtSize(TextScale);   // force menu's own scale, independent of Screen's
        // Every menu-style dialog in this project (the main menu itself,
        // showInfoBox(), showDeviceList() in boardui.cpp) calls
        // MenuFrame::draw() as its own first step, so putting this here
        // covers all of them with one call: menus always use the chip's
        // own built-in CGROM font (a standard, legible typeface), never
        // the custom hp82163_font.hpp glyphs Screen's own HP-41-emulated
        // text uses (see Screen.cpp's own selectCustomFont() call) --
        // matches this project's own "reassert explicitly, don't rely on
        // whichever mode was last active" convention (see txtSize()
        // just above, and Screen.cpp's own comment on why).
        d->selectBuiltinFont();
        // The four small corners outside the rounded shape show what's
        // BEHIND the box, so it really looks round (they used to be
        // filled black). On the 7" panel the box is drawn on the menu
        // layer, shown as a PIP window on top of the panel: what's behind
        // is first copied there from the panel (BTE, inside the display
        // chip) -- otherwise the corners would show stale layer memory.
        if (d->canvasAddr() == LT7683::kMenuLayerAddr) {
            d->copyLayerRegion(0, SCREEN_MAX_X, static_cast<std::int16_t>(x), static_cast<std::int16_t>(y),
                               LT7683::kMenuLayerAddr, SCREEN_MAX_X, static_cast<std::int16_t>(x),
                               static_cast<std::int16_t>(y), static_cast<std::uint16_t>(w),
                               static_cast<std::uint16_t>(h));
        } else {
            d->fillRect(x, y, w, h, 0x0000);   // (another layer: no panel picture to show)
        }
        d->fillRoundRect(x, y, w, h, CornerRadius, Yellow);
        const int innerRadius = CornerRadius > BorderThickness
                                     ? CornerRadius - BorderThickness : 0;
        d->fillRoundRect(x + BorderThickness, y + BorderThickness,
                         w - 2 * BorderThickness, h - 2 * BorderThickness,
                         innerRadius, 0x0000);
    }

    // How the chip's own fillRoundRect() rounds a corner: out[c][row] =
    // how many pixels of that row of the CornerRadius-square corner are
    // OUTSIDE the box, counted from the box's side (c: 0 top-left, 1
    // top-right, 2 bottom-left, 3 bottom-right; rows from the corner's
    // top). Measured once -- a box drawn on a scratch layer (never shown)
    // and read back -- so it matches what draw() drew, pixel for pixel.
    // Canvas and Active Window as they were.
    struct CornerShape {
        std::uint8_t outside[4][CornerRadius];
        int dx = 0, dy = 0;     // the chip's box ends this many pixels right / below x+w-1 / y+h-1
    };
    inline const CornerShape& cornerOutside(DisplayDriver* d) {
        static CornerShape cs;
        static bool known = false;
        if (known) return cs;
        constexpr int r = CornerRadius, size = 4 * CornerRadius, area = size + 8;
        constexpr std::uint16_t kMark = 0x0821;               // (not Yellow)
        const std::uint32_t canvas = d->canvasAddr();
        const std::uint16_t stride = d->canvasStride();
        std::uint16_t awX, awY, awW, awH;
        d->widenActiveWindowForLinearAccess(&awX, &awY, &awW, &awH);
        d->beginLayerDraw(LT7683::kBteLayer2Addr);
        d->setActiveWindow(0, 0, SCREEN_MAX_X - 1, SCREEN_MAX_Y - 1);
        d->fillRect(0, 0, area, area, kMark);
        d->fillRoundRect(0, 0, size, size, r, Yellow);
        static std::uint16_t px[area][area];
        for (int y = 0; y < area; ++y) d->readRow565(0, static_cast<std::int16_t>(y), area, px[y]);
        // Where the box really ends (in case the chip draws one more row or column)
        int right = 0, bottom = 0;
        for (int y = 0; y < area; ++y)
            for (int x = 0; x < area; ++x)
                if (px[y][x] != kMark) { right = std::max(right, x); bottom = std::max(bottom, y); }
        cs.dx = right - (size - 1);
        cs.dy = bottom - (size - 1);
        for (int row = 0; row < r; ++row) {
            const std::uint16_t* top = px[row];
            const std::uint16_t* bot = px[bottom - r + 1 + row];
            int n[4] = { 0, 0, 0, 0 };
            while (n[0] < r && top[n[0]] == kMark) ++n[0];
            while (n[1] < r && top[right - n[1]] == kMark) ++n[1];
            while (n[2] < r && bot[n[2]] == kMark) ++n[2];
            while (n[3] < r && bot[right - n[3]] == kMark) ++n[3];
            for (int c = 0; c < 4; ++c) cs.outside[c][row] = static_cast<std::uint8_t>(n[c]);
        }
        if (canvas == 0) d->endOverlayDraw();
        else d->beginLayerDraw(canvas, stride);
        d->restoreActiveWindow(awX, awY, awW, awH);
        known = true;
        return cs;
    }

    // The four corners outside the rounded shape show what's behind the box
    // (see draw()) -- copied when the box was drawn. When that changes with
    // the box still open (Paper black/white, the text colour ...), this
    // copies just those pixels again from the panel (BTE, one short row
    // at a time, as cornerOutside() says): the frame itself isn't drawn
    // again, so nothing of it can change. For a box on the menu layer.
    inline void refreshCorners(DisplayDriver* d, int x, int y, int w, int h) {
        const CornerShape& cs = cornerOutside(d);
        const int r = CornerRadius;
        for (int c = 0; c < 4; ++c)
            for (int row = 0; row < r; ++row) {
                const int len = cs.outside[c][row];
                if (len == 0) continue;
                const auto px = static_cast<std::int16_t>((c & 1) ? x + w + cs.dx - len : x);
                const auto py = static_cast<std::int16_t>((c & 2) ? y + h + cs.dy - r + row : y + row);
                d->copyLayerRegion(0, SCREEN_MAX_X, px, py, LT7683::kMenuLayerAddr, SCREEN_MAX_X, px, py,
                                   static_cast<std::uint16_t>(len), 1);
            }
    }

    // A box with its title band (title upper-cased), at any place and size
    // -- drawBox() in UiDialog is this at the standard place
    inline void drawTitled(DisplayDriver* d, int x, int y, int w, int h, const std::string& title) {
        draw(d, x, y, w, h);
        // The band follows the frame's inner rounding at the top (a plain
        // rectangle stuck out past the outer rounding there, a few yellow
        // pixels in the corners): rounded, then its lower rounding filled
        // back black (the rows start well below it)
        const int innerRadius = CornerRadius > BorderThickness ? CornerRadius - BorderThickness : 0;
        d->fillRoundRect(x + BorderThickness, y + BorderThickness, w - 2 * BorderThickness,
                         TitleH - BorderThickness + innerRadius, innerRadius, Yellow);
        d->fillRect(x + BorderThickness, y + TitleH, w - 2 * BorderThickness, innerRadius, 0x0000);
        std::string t = title.empty() ? std::string("HIPI") : title;
        for (char& c : t) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
#if HIPI_MENU_TITLE_HP41FONT
        drawGlyphText(d, x + 16, y + (TitleH - kMenuTitleFont.height) / 2 + 1, kMenuTitleFont,
                      t.c_str(), 0x0000, Yellow);
        d->selectBuiltinFont();
#else
        d->txtSize(0);
        d->txtColor(0x0000, Yellow);
        d->txtSetCursor(static_cast<std::uint16_t>(x + 16), static_cast<std::uint16_t>(y + 8));
        d->txtWrite(t.c_str());
#endif
        d->txtSize(TextScale);
    }
}  // namespace MenuFrame

}  // namespace hipi
