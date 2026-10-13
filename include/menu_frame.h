// menu_frame.h -- the look of every menu and dialog: a black box with a
// thick, rounded yellow frame and the title in a yellow band at the top,
// written in the HP-41's display font (or the built-in font, see
// HIPI_MENU_TITLE_HP41FONT). Shared by UiDialog (uidialog.hpp), the boxes
// in boardui.cpp and the Create media dialogs (media_create_ui.h).
#pragma once
#include "display_config.h"
#include "glyph_font.h"
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

    // The RA8875 only has one hardware text-scale register, shared by
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
        // chip) -- otherwise the corners would show stale layer memory. On
        // the 5" panel the box is drawn straight onto the screen.
#ifdef DISPLAY_7INCH
        if (d->canvasAddr() == LT7683::kMenuLayerAddr) {
            d->copyLayerRegion(0, SCREEN_MAX_X, static_cast<std::int16_t>(x), static_cast<std::int16_t>(y),
                               LT7683::kMenuLayerAddr, SCREEN_MAX_X, static_cast<std::int16_t>(x),
                               static_cast<std::int16_t>(y), static_cast<std::uint16_t>(w),
                               static_cast<std::uint16_t>(h));
        } else {
            d->fillRect(x, y, w, h, 0x0000);   // (another layer: no panel picture to show)
        }
#endif
        d->fillRoundRect(x, y, w, h, CornerRadius, Yellow);
        const int innerRadius = CornerRadius > BorderThickness
                                     ? CornerRadius - BorderThickness : 0;
        d->fillRoundRect(x + BorderThickness, y + BorderThickness,
                         w - 2 * BorderThickness, h - 2 * BorderThickness,
                         innerRadius, 0x0000);
    }

    // A box with its title band (title upper-cased), at any place and size
    // -- drawBox() in UiDialog is this at the standard place
    inline void drawTitled(DisplayDriver* d, int x, int y, int w, int h, const std::string& title) {
        draw(d, x, y, w, h);
        d->fillRect(x + BorderThickness, y + BorderThickness, w - 2 * BorderThickness,
                    TitleH - BorderThickness, Yellow);
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
