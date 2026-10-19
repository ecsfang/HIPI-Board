// devicelist.cpp -- see devicelist.h
#include "devicelist.h"
#include "menu_frame.h"
#include <algorithm>
#include <cstdio>

namespace hipi {
namespace devlist {

namespace {

std::int16_t i16(int v) { return static_cast<std::int16_t>(v); }

void drawCheckbox(DisplayDriver* d, int x, int y, bool on) {
    const std::uint16_t c = on ? 0xFFFF : 0x8410;
    d->rect(i16(x), i16(y), kCheck, kCheck, c);
    d->rect(i16(x + 1), i16(y + 1), kCheck - 2, kCheck - 2, c);
    if (!on) return;
    for (int t = -1; t <= 1; ++t) {                   // an X, like "[X]"
        d->line(i16(x + 6 + t), i16(y + 6), i16(x + kCheck - 7 + t), i16(y + kCheck - 7), 0xFFFF);
        d->line(i16(x + 6 + t), i16(y + kCheck - 7), i16(x + kCheck - 7 + t), i16(y + 6), 0xFFFF);
    }
}

void selectRowFont(DisplayDriver* d) {
    d->selectBuiltinFont();
    d->selectBuiltinFontSize(1);                      // 12x24
    d->txtSize(0);
}

}  // namespace

void drawFrame(DisplayDriver* d) {
    MenuFrame::drawTitled(d, kX, kY, kW, kH, "HP-IL DEVICES");
    const int x = kX + kW - MenuFrame::BorderThickness - kCloseW - 6, y = kY + MenuFrame::BorderThickness + 3;
    const int w = kCloseW, h = kTitleH - MenuFrame::BorderThickness - 6;
    d->fillRoundRect(i16(x), i16(y), w, h, 5, 0x0000);
    const int cx = x + w / 2, cy = y + h / 2;
    for (int t = -1; t <= 1; ++t) {
        d->line(i16(cx - 6 + t), i16(cy - 6), i16(cx + 6 + t), i16(cy + 6), 0xFFFF);
        d->line(i16(cx - 6 + t), i16(cy + 6), i16(cx + 6 + t), i16(cy - 6), 0xFFFF);
    }
}

void drawRows(DisplayDriver* d, const std::vector<DeviceInfo>& list, int offset) {
    const int rowsBottom = kY + kH - MenuFrame::CornerRadius;
    // (inset by the corner radius: a plain rectangle mustn't cut the frame's rounded corners)
    d->fillRect(i16(kX + MenuFrame::CornerRadius), i16(kRowsTop - 4), kW - 2 * MenuFrame::CornerRadius,
                i16(rowsBottom - (kRowsTop - 4)), 0x0000);
    selectRowFont(d);
    const int textH = kRowPitch - 10;                 // the glyphs' height (24 / 16)
    char buf[96];
    for (int i = 0; i < kRows; ++i) {
        const std::size_t idx = static_cast<std::size_t>(offset + i);
        if (idx >= list.size()) break;
        const DeviceInfo& info = list[idx];
        const int y = kRowsTop + i * kRowPitch;
        drawCheckbox(d, kX + 20, y + (textH - kCheck) / 2, info.enabled);
        if (info.extMode[0] != '\0' && info.extDevices > 0) {
            // PILBox: the link mode (COFF/COFI/CON, or TDIS when no PC
            // program is connected) + the PC-side devices and their addresses
            std::snprintf(buf, sizeof(buf), "%7s  %-10s %s: %d (addr %d-%d)", "--", info.devName, info.extMode,
                          info.extDevices, info.extFirstAddr, info.extFirstAddr + info.extDevices - 1);
        } else if (info.extMode[0] != '\0') {
            std::snprintf(buf, sizeof(buf), "%7s  %-10s %s", "--", info.devName, info.extMode);
        } else if (info.addr < 0) {
            std::snprintf(buf, sizeof(buf), "%7s  %s", "--", info.devName);
        } else {
            // "addr NN": room for two-digit addresses (up to 30)
            std::snprintf(buf, sizeof(buf), "addr %2d  %-10s 0x%02X", info.addr, info.devName, info.sai);
        }
        d->txtColor(info.enabled ? 0xFFFF : 0x8410, 0x0000);   // switched-off devices dimmer
        d->txtSetCursor(static_cast<std::uint16_t>(kX + 20 + kCheck + 18), static_cast<std::uint16_t>(y));
        d->txtWrite(buf);
    }
    // More above / below: arrows at the right, in the rows' own margin
    const int ax = kX + kW - 34;
    const bool moreAbove = offset > 0;
    const bool moreBelow = offset + kRows < static_cast<int>(list.size());
    if (moreAbove)
        d->fillTriangle(i16(ax), i16(kRowsTop + 14), i16(ax + 16), i16(kRowsTop + 14), i16(ax + 8), i16(kRowsTop + 2),
                        MenuFrame::Yellow);
    if (moreBelow)
        d->fillTriangle(i16(ax), i16(rowsBottom - 18), i16(ax + 16), i16(rowsBottom - 18), i16(ax + 8),
                        i16(rowsBottom - 6), MenuFrame::Yellow);
    d->selectBuiltinFont();                           // (back to the 8x16 the rest expects)
}

bool isClose(int x, int y) {
    const int right = kX + kW;
    return x >= right - kCloseW - 24 && x < right + 8 && y + 10 >= kY && y < kY + kTitleH + 14;
}

int checkboxAt(int x, int y, int offset, int count) {
    // The box and a margin around it: a finger-sized target
    if (x < kX + 4 || x >= kX + 20 + kCheck + 16) return -1;
    if (y < kRowsTop - 4) return -1;
    const int row = (y - (kRowsTop - 4)) / kRowPitch;
    if (row >= kRows) return -1;
    const int idx = offset + row;
    return idx < count ? idx : -1;
}

int maxOffset(int count) { return std::max(0, count - kRows); }

}  // namespace devlist
}  // namespace hipi
