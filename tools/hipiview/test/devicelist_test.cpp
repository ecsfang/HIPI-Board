// devicelist_test.cpp -- the HP-IL DEVICES list (a touch in the bottom-left
// corner) on the PC: the real devicelist.cpp with the LT7683 driver against
// the emulated chip. Saves:
//
//   devicelist_1.png   eleven devices (scrolls), one switched off, PILBOX
//   devicelist_2.png   scrolled to the end
//
//   make devicelist_test && ./devicelist_test   (in tools/hipiview)
#include "LT7683.hpp"
#include "devicelist.h"
#include "hp82163_font.hpp"
#include "../lt7683_emu.hpp"
#include "../png.hpp"
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

LT7683Emu chip;

class ChipTransport : public hipi::RA8875Transport {
public:
    void csLow() override { first_ = true; }
    void csHigh() override {}
    void rstLow() override {}
    void rstHigh() override {}
    void spiTransfer(const std::uint8_t* tx, std::uint8_t* rx, std::size_t len) override {
        for (std::size_t i = 0; i < len; ++i) {
            const std::uint8_t b = tx ? tx[i] : 0;
            if (first_) { type_ = b & 0xC0; first_ = false; if (rx) rx[i] = 0; continue; }
            std::uint8_t r = 0;
            switch (type_) {
            case 0x00: chip.hostCmd(b); break;
            case 0x80: chip.hostWrite(b); break;
            case 0xC0: r = chip.hostRead(); break;
            default:   r = chip.hostStatus(); break;
            }
            if (rx) rx[i] = r;
        }
    }
    void delayMs(std::uint32_t) override {}
    void delayUs(std::uint32_t) override {}
private:
    bool first_ = true;
    std::uint8_t type_ = 0;
};

int failures = 0;
std::vector<std::uint32_t> panel(1024 * 600);
void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}
void snapshot(const char* png) {
    chip.render(panel.data(), 0.0);
    writePng(png, panel.data(), 1024, 600);
    std::printf("     saved %s\n", png);
}
void makeChipFonts() {
    std::uint8_t f[256][16];
    for (int c = 0; c < 256; ++c) {
        if (c >= 32 && c < 127) std::memcpy(f[c], hipi::font[c - 32], 16);
        else std::memset(f[c], 0, 16);
    }
    chip.setBuiltinFont(f);
    static std::uint16_t f24[256][24];
    for (int c = 0; c < 256; ++c)
        for (int r = 0; r < 24; ++r) {
            std::uint16_t row = 0;
            for (int x = 0; x < 12; ++x)
                if (f[c][r * 16 / 24] & (0x80 >> (x * 8 / 12))) row = static_cast<std::uint16_t>(row | (0x8000 >> x));
            f24[c][r] = row;
        }
    chip.setBuiltinFont24(f24);
}
DeviceInfo dev(const char* name, int addr, std::uint8_t sai, bool on) {
    DeviceInfo d{};
    std::snprintf(d.devName, sizeof(d.devName), "%s", name);
    d.addr = addr;
    d.sai = sai;
    d.enabled = on;
    return d;
}

}  // namespace

int main() {
    ChipTransport t;
    hipi::LT7683 d(t, SCREEN_MAX_X, SCREEN_MAX_Y);
    d.begin();
    makeChipFonts();
    std::vector<DeviceInfo> list = {
        dev("PILBOX", -1, 0, true), dev("TFDISPLAY", 1, 0x30, true), dev("TFDRIVE", 2, 0x10, true),
        dev("TFPRINT", -1, 0x20, false), dev("TFPLOT", 3, 0x60, true), dev("TFLEDS", 4, 0xEE, true),
        dev("TFPIXEL", 5, 0xED, true), dev("TFTERM", -1, 0, false), dev("TFEXTRA1", 6, 0x00, true),
        dev("TFEXTRA2", 7, 0x00, true), dev("TFEXTRA3", 8, 0x00, true) };
    std::snprintf(list[0].extMode, sizeof(list[0].extMode), "COFF");
    list[0].extDevices = 3;
    list[0].extFirstAddr = 9;

    d.fillRect(0, 0, SCREEN_MAX_X, SCREEN_MAX_Y, 0x2104);
    hipi::devlist::drawFrame(&d);
    hipi::devlist::drawRows(&d, list, 0);
    snapshot("devicelist_1.png");
    using namespace hipi::devlist;
    std::printf("     box %d,%d %dx%d, rows from y %d, %d px a row\n", kX, kY, kW, kH, kRowsTop, kRowPitch);
    check(kRows == 9, "nine rows, as before");
    check(kY >= 0 && kY + kH <= SCREEN_MAX_Y && kX + kW <= SCREEN_MAX_X, "the box fits the panel");
    // The on/off boxes: each row's, a finger-sized target
    const int cx = kX + 20 + kCheck / 2;
    check(checkboxAt(cx, kRowsTop + 5, 0, 11) == 0 && checkboxAt(cx, kRowsTop + 3 * kRowPitch + 5, 0, 11) == 3,
          "a touch on a row's box: that device");
    check(checkboxAt(cx - 20, kRowsTop + kRowPitch + 2, 0, 11) == 1 && checkboxAt(cx + 25, kRowsTop + kRowPitch + 2, 0, 11) == 1,
          "... a bit beside it too");
    check(checkboxAt(cx + 200, kRowsTop + 5, 0, 11) == -1, "the row's text: not the box");
    check(checkboxAt(cx, kRowsTop + 2 * kRowPitch + 5, 2, 11) == 4, "scrolled: the row's device in the list");
    check(checkboxAt(cx, kRowsTop + 8 * kRowPitch + 5, 4, 11) == -1, "below the last device: nothing");
    check(isClose(kX + kW - 30, kY + 15) && !isClose(kX + 100, kY + 15), "the close button in the title band");
    // The box drawn: white X in TFDISPLAY's box, grey empty one for TFPRINT
    chip.render(panel.data(), 0.0, false);
    auto px = [&](int x, int y) { return panel[static_cast<std::size_t>(y) * 1024 + x] & 0xFFFFFF; };
    const int boxY = kRowsTop + (kRowPitch - 10 - kCheck) / 2;
    const int midOn = px(kX + 20 + kCheck / 2, boxY + kRowPitch + kCheck / 2);
    const int midOff = px(kX + 20 + kCheck / 2, boxY + 3 * kRowPitch + kCheck / 2);
    check(midOn == 0xFFFFFF && midOff == 0, "switched on: an X in the box; off: empty");
    check(maxOffset(11) == 2, "eleven devices: two to scroll");
    hipi::devlist::drawRows(&d, list, maxOffset(11));
    snapshot("devicelist_2.png");

    std::printf("%s\n", failures ? "FAILED" : "ALL PASSED");
    return failures ? 1 : 0;
}
