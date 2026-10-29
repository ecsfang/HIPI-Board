// page_chrome.h -- what every page has (views_isPage(): Hardware test, Loop
// map, Media map, Character table, Analyzer, HP-IL signals): the close
// button at the right of the title bar, and the menu button in the bottom
// right corner -- the only place where a touch brings up the button strip
// on a page (on the other views the whole right edge does). A page keeps
// both places free of anything else; the corner below kPageContentBottom.
#pragma once
#include "display_config.h"
#include "menu_frame.h"
#include <cstdint>

namespace hipi {

// The close button: in the title bar's right end (a page's title bar is
// 20..26 px high), touched a little below the bar too
constexpr int kPageCloseW = 64;
constexpr int kPageCloseTouchH = 38;
// The menu button: bottom right; a page's content ends above it
constexpr int kPageMenuX = SCREEN_MAX_X - 64, kPageMenuY = SCREEN_MAX_Y - 38;
constexpr int kPageMenuW = 58, kPageMenuH = 34;
constexpr int kPageContentBottom = kPageMenuY - 6;            // (556) nothing below this ...
constexpr int kPageContentRight = kPageMenuX - 8;             // ... right of this (952)
// ... and its touch area, a finger's size around it (the corner below and
// right of the page's content)
constexpr int kPageMenuTouchX = SCREEN_MAX_X - 80, kPageMenuTouchY = kPageContentBottom;

inline bool page_closeHit(int x, int y) {
    return x >= SCREEN_MAX_X - kPageCloseW && x < SCREEN_MAX_X && y >= 0 && y < kPageCloseTouchH;
}
inline bool page_menuHit(int x, int y) {
    return x >= kPageMenuTouchX && x < SCREEN_MAX_X && y >= kPageMenuTouchY && y < SCREEN_MAX_Y;
}

// The close button: black on the yellow title bar (titleH high), a white X
inline void page_drawClose(DisplayDriver* d, int titleH) {
    auto i16 = [](int v) { return static_cast<std::int16_t>(v); };
    const int x = SCREEN_MAX_X - kPageCloseW + 6, y = 2, w = kPageCloseW - 8, h = titleH - 4;
    d->fillRoundRect(i16(x), i16(y), i16(w), i16(h), 6, 0x0000);
    const int cx = x + w / 2, cy = y + h / 2, a = h >= 20 ? 7 : 5;
    for (int t = -1; t <= 1; ++t) {
        d->line(i16(cx - a + t), i16(cy - a), i16(cx + a + t), i16(cy + a), 0xFFFF);
        d->line(i16(cx - a + t), i16(cy + a), i16(cx + a + t), i16(cy - a), 0xFFFF);
    }
}

// The menu button: a "hamburger" (three bars) in a dark rounded box with
// the menus' yellow frame
inline void page_drawMenu(DisplayDriver* d) {
    auto i16 = [](int v) { return static_cast<std::int16_t>(v); };
    const int x = kPageMenuX, y = kPageMenuY, w = kPageMenuW, h = kPageMenuH;
    d->fillRect(i16(x - 4), i16(y - 4), i16(w + 8), i16(h + 8), 0x0000);
    d->fillRoundRect(i16(x), i16(y), i16(w), i16(h), 8, MenuFrame::Yellow);
    d->fillRoundRect(i16(x + 2), i16(y + 2), i16(w - 4), i16(h - 4), 6, 0x10A2);
    for (int k = 0; k < 3; ++k) d->fillRect(i16(x + 16), i16(y + 8 + k * 8), i16(w - 32), 3, MenuFrame::Yellow);
}

}  // namespace hipi
