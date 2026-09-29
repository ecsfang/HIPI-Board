#define MODULE "BOOT"
#include "bootscreen.h"
#include "sd_paths.h"
#include "uidialog.hpp"     // MenuFrame (same look as the menus)
#include "bmp_loader.hpp"   // drawBmpAt(), peekBmpDimensions()
#include "usb_serial.h"
#include "pico/time.h"
#include <cstdio>
#include <cstring>

namespace hipi {

#ifdef DISPLAY_7INCH

namespace {

DisplayDriver* d_ = nullptr;

// RGB565 colours
constexpr std::uint16_t kYellow = MenuFrame::Yellow;
constexpr std::uint16_t kBlack  = 0x0000;
constexpr std::uint16_t kWhite  = 0xFFFF;
constexpr std::uint16_t kGrey   = 0x6B6D;   // secondary text
constexpr std::uint16_t kDark   = 0x31A6;   // "off" outlines / dots
constexpr std::uint16_t kGreen  = 0x2E48;
constexpr std::uint16_t kRed    = 0xE185;

// Layout (1024 x 600)
constexpr int kW = SCREEN_MAX_X, kH = SCREEN_MAX_Y;
constexpr int kCardY = 180, kCardH = 330;
constexpr int kStatusX = 30, kStatusW = 470;
constexpr int kDevX = 520, kDevW = kW - 520 - 30;
constexpr int kRowY0 = 204, kRowPitch = 40, kMaxRows = 7;
constexpr int kBarX = 40, kBarY = 572, kBarW = kW - 80, kBarH = 6;

int rows_ = 0;
bool footerDrawn_ = false;

// Draw into the menu layer (shown as PIP-1), with the full-panel Active
// Window -- both restored afterwards, whatever start-up had set
template <typename F>
void onOverlay(F&& draw) {
    std::uint16_t ax, ay, aw, ah;
    d_->widenActiveWindowForLinearAccess(&ax, &ay, &aw, &ah);
    d_->beginOverlayDraw();
    d_->setActiveWindow(0, 0, kW - 1, kH - 1);
    d_->selectBuiltinFont();
    draw();
    d_->endOverlayDraw();
    d_->restoreActiveWindow(ax, ay, aw, ah);
}

// Text; fillRect() changes the chip's foreground colour, so the colour is
// always set right before writing
void text(int x, int y, const char* s, std::uint8_t scale, std::uint16_t fg, std::uint16_t bg = kBlack) {
    d_->txtSize(scale);
    d_->txtColor(fg, bg);
    d_->txtSetCursor(static_cast<std::uint16_t>(x), static_cast<std::uint16_t>(y));
    d_->txtWrite(s);
}

// " LABEL " tab on a card's top edge, black on yellow
void tab(int x, int y, const char* label) {
    const int w = static_cast<int>(std::strlen(label)) * 8 + 16;
    d_->fillRect(x, y, w, 18, kYellow);
    text(x + 8, y + 1, label, 0, kBlack, kYellow);
}

void card(int x, int w, const char* label) {
    d_->roundRect(x, kCardY, w, kCardH, 12, kYellow);
    d_->roundRect(x + 1, kCardY + 1, w - 2, kCardH - 2, 11, kYellow);
    tab(x + 16, kCardY - 9, label);
}

}  // namespace

void bootscreen_begin(DisplayDriver* display, const char* version) {
    d_ = display;
    rows_ = 0;
    footerDrawn_ = false;
    onOverlay([&] {
        d_->fillRect(0, 0, kW, kH, kBlack);
        MenuFrame::draw(d_, 8, 8, kW - 16, kH - 16);
        d_->selectBuiltinFont();
        // Logo (if on the SD card), name, version
        std::uint16_t lw = 0, lh = 0;
        const bool logo = peekBmpDimensions(HIPI_PATH(HIPI_DIR_RESOURCES, "logo.bmp"), lw, lh) &&
                          drawBmpAt(d_, HIPI_PATH(HIPI_DIR_RESOURCES, "logo.bmp"), 34, 30, nullptr, &lw, &lh);
        const int tx = logo ? 34 + lw + 22 : 40;
        text(tx, 30, "HIPI", 3, kYellow);
        text(tx + 2, 104, "HP-IL Pico Interface", 1, kWhite);
        char buf[32];
        std::snprintf(buf, sizeof(buf), "v%s", version);
        const int vw = static_cast<int>(std::strlen(buf)) * 24 + 24;
        d_->fillRect(kW - 60 - vw, 36, vw, 58, kYellow);
        text(kW - 60 - vw + 12, 41, buf, 2, kBlack, kYellow);
        text(kW - 60 - vw, 112, "7\" 1024x600  RP2350", 0, kGrey);
        d_->fillRect(30, 158, kW - 60, 2, kYellow);
        card(kStatusX, kStatusW, "STATUS");
        card(kDevX, kDevW, "DEVICES ON THE LOOP");
    });
    d_->showPipOverlay(0, 0, kW, kH);
}

void bootscreen_row(const char* label, BootState state, const char* value) {
    if (d_ == nullptr || rows_ >= kMaxRows) return;
    const int y = kRowY0 + rows_ * kRowPitch;
    ++rows_;
    onOverlay([&] { text(kStatusX + 52, y, label, 1, kWhite); });
    // Short "working on it" animation before the result appears
    for (int i = 1; i <= 3; ++i) {
        onOverlay([&] { text(kStatusX + 220 + (i - 1) * 16, y, ".", 1, kGrey); });
        sleep_ms(70);
    }
    onOverlay([&] {
        d_->fillRect(kStatusX + 220, y, kStatusW - 230, 34, kBlack);
        const std::uint16_t dot = state == BootState::Ok   ? kGreen
                                : state == BootState::Fail ? kRed
                                : state == BootState::Off  ? kDark : kYellow;
        d_->fillCircle(kStatusX + 30, y + 16, 7, dot);
        // Long values (file names) in the small font so they fit
        const bool small = std::strlen(value) > 14;
        text(kStatusX + 220, small ? y + 8 : y, value, small ? 0 : 1,
             state == BootState::Fail ? kRed : kWhite);
    });
    LOGF("\r\n * Boot: %-10s %s", label, value);
}

void bootscreen_devices(const std::vector<CDevice*>& devices) {
    if (d_ == nullptr) return;
    const int n = static_cast<int>(devices.size());
    const int perCol = (n + 1) / 2;
    const int pitch = perCol > 0 ? std::min(62, 270 / perCol) : 62;
    onOverlay([&] {
        for (int i = 0; i < n; ++i) {
            const int x = kDevX + 26 + (i / perCol) * 222;
            const int y = kCardY + 30 + (i % perCol) * pitch;
            char num[12];
            std::snprintf(num, sizeof(num), "%d", i + 1);
            text(x, y, num, 1, kGrey);
            const bool on = devices[static_cast<std::size_t>(i)]->enabled();
            if (on) {
                d_->fillRoundRect(x + 30, y, 170, 28, 6, kYellow);
                text(x + 40, y + 6, devices[static_cast<std::size_t>(i)]->name(), 0, kBlack, kYellow);
            } else {
                d_->roundRect(x + 30, y, 170, 28, 6, kDark);
                text(x + 40, y + 6, devices[static_cast<std::size_t>(i)]->name(), 0, kGrey);
            }
        }
        text(kDevX + 26, kCardY + kCardH - 32, "yellow = on    grey = off", 0, kGrey);
    });
}

void bootscreen_progress(float fraction) {
    if (d_ == nullptr) return;
    if (fraction < 0) fraction = 0;
    if (fraction > 1) fraction = 1;
    onOverlay([&] {
        if (!footerDrawn_) {
            footerDrawn_ = true;
            text(40, 526, "Ready.  Touch the screen to start.", 1, kWhite);
            d_->roundRect(kBarX - 1, kBarY - 1, kBarW + 2, kBarH + 2, 2, kDark);
        }
        const int fill = static_cast<int>(kBarW * fraction);
        if (fill > 0) d_->fillRect(kBarX, kBarY, fill, kBarH, kYellow);
    });
}

void bootscreen_end() {
    if (d_ == nullptr) return;
    d_->hidePipOverlay();
    d_ = nullptr;
}

#else  // 5" panel: start-up keeps the classic splash + text summary

void bootscreen_begin(DisplayDriver*, const char*) {}
void bootscreen_row(const char*, BootState, const char*) {}
void bootscreen_devices(const std::vector<CDevice*>&) {}
void bootscreen_progress(float) {}
void bootscreen_end() {}

#endif

}  // namespace hipi
