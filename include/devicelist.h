// devicelist.h -- the HP-IL DEVICES list (a touch in the bottom-left
// corner, boardui.cpp): every one of HIPI's devices in loop order with its
// address, accessory ID and whether it's switched on -- a box in front of
// it to switch it on or off. Only the drawing and where things are; what a
// touch does is boardui.cpp's. Apart from boardui.cpp so it can be tried
// on the PC (tools/hipiview/test/devicelist_test.cpp).
//
// The 12x24 built-in font, 34 px a row. Nine rows; more scroll.
#pragma once
#include "display_config.h"
#include "device_info.h"
#include <string>
#include <vector>

namespace hipi {
namespace devlist {

constexpr int kX = 60, kW = 640, kRowPitch = 34, kCharW = 12, kCheck = 26;
constexpr int kRows = 9;
constexpr int kTitleH = 30;
constexpr int kRowsTopGap = 14;                 // title band -> first row
constexpr int kH = kTitleH + kRowsTopGap + kRows * kRowPitch + 24;
constexpr int kY = (SCREEN_MAX_Y - kH) / 2;      // the box (with its title band)
constexpr int kRowsTop = kY + kTitleH + kRowsTopGap;
constexpr int kCloseW = 44;                      // the close button in the title band

// The box, its title band and close button
void drawFrame(DisplayDriver* d);
// The rows from `offset` on (and the more-above / more-below arrows)
void drawRows(DisplayDriver* d, const std::vector<DeviceInfo>& list, int offset);
// A row's text: "addr  6  TFI2C      0x52 (82)" -- the accessory ID in hex
// and decimal (the HP-71's DEVADDR("%82") takes the decimal one)
std::string rowText(const DeviceInfo& info);
// The close button (with a margin around it)
bool isClose(int x, int y);
// The on/off box of a row (with a margin): its index in the list, or -1
int checkboxAt(int x, int y, int offset, int count);
// The largest offset (scrolling)
int maxOffset(int count);

}  // namespace devlist
}  // namespace hipi
