#define MODULE "DUMP"
#include "screendump.h"
#include "sd_paths.h"
#include "config.hpp"
#include "usb_msc.h"
#include "usb_serial.h"   // LOGF
#include "ff.h"
#include "pico/time.h"
#include <cstdio>
#include <cstdint>
#include <algorithm>

extern hipi::Config config;

namespace hipi {

namespace {
volatile int pendingRequest_ = 0;   // 0 = none, 1 = plain, 2 = with overlays
}

void screendump_request(bool withOverlays) {
    pendingRequest_ = withOverlays ? 2 : 1;
}

bool screendump_takePending(bool& withOverlays) {
    const int r = pendingRequest_;
    if (r == 0) return false;
    pendingRequest_ = 0;
    withOverlays = (r == 2);
    return true;
}

#ifdef DISPLAY_7INCH

namespace {

void put16(std::uint8_t* p, std::uint16_t v) { p[0] = v & 0xFF; p[1] = v >> 8; }
void put32(std::uint8_t* p, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}

constexpr unsigned kMaxTries = 10000;   // give up searching for a free name

}  // namespace

// Copies the part of PIP window `pip` that covers screen row y into px[]
// (read from the PIP's own source layer). Leaves canvas and Active Window
// set for the live panel again.
void overlayPipRow(DisplayDriver* d, int pip, int y, std::uint16_t* px) {
    const LT7683::PipState& s = d->pipState(pip);
    if (!s.on || d->pipsBlanked() || y < s.dstY || y >= s.dstY + s.h) return;   // (blanked: a screen saver)
    const int x0 = std::max<int>(0, s.dstX);
    const int x1 = std::min<int>(SCREEN_MAX_X, s.dstX + s.w);
    if (x1 <= x0) return;
    static std::uint16_t seg[SCREEN_MAX_X];
    d->beginLayerDraw(s.addr, s.stride);
    d->setActiveWindow(0, 0, s.stride - 1, SCREEN_MAX_Y - 1);
    d->readRow565(static_cast<std::int16_t>(s.srcX + (x0 - s.dstX)),
                  static_cast<std::int16_t>(s.srcY + (y - s.dstY)),
                  static_cast<std::uint16_t>(x1 - x0), seg);
    d->endOverlayDraw();
    d->setActiveWindow(0, 0, SCREEN_MAX_X - 1, SCREEN_MAX_Y - 1);
    for (int x = x0; x < x1; ++x) px[x] = seg[x - x0];
}

bool screendump_save(DisplayDriver* d, std::string& message, bool withOverlays) {
    if (usbMscModeActive()) {
        message = "SD card in use by PC";
        return false;
    }

    // First free "screenshots/screendump_<n>.bmp", starting at the saved
    // counter (the folder is created if it isn't there yet)
    f_mkdir(HIPI_DIR_SCREENSHOTS);          // FR_EXIST if it is -- fine
    unsigned n = config.screendumpNext();
    char name[48];
    FILINFO info;
    unsigned tries = 0;
    for (;; ++n, ++tries) {
        std::snprintf(name, sizeof(name), HIPI_DIR_SCREENSHOTS "/screendump_%u.bmp", n);
        if (f_stat(name, &info) == FR_NO_FILE) break;
        if (tries >= kMaxTries) {
            message = "No free file name";
            return false;
        }
    }

    FIL file;
    if (f_open(&file, name, FA_WRITE | FA_CREATE_NEW) != FR_OK) {
        message = "Can't create file";
        return false;
    }

    // 24-bit uncompressed BMP, bottom-up (the format bmp_loader.hpp reads)
    constexpr std::uint32_t W = SCREEN_MAX_X, H = SCREEN_MAX_Y;
    constexpr std::uint32_t rowBytes = (W * 3 + 3) & ~3u;
    std::uint8_t hdr[54] = { 'B', 'M' };
    put32(hdr + 2, 54 + rowBytes * H);   // file size
    put32(hdr + 10, 54);                 // pixel data offset
    put32(hdr + 14, 40);                 // BITMAPINFOHEADER size
    put32(hdr + 18, W);
    put32(hdr + 22, H);                  // positive = bottom-up
    put16(hdr + 26, 1);                  // planes
    put16(hdr + 28, 24);                 // bits per pixel
    put32(hdr + 34, rowBytes * H);       // image size
    put32(hdr + 38, 2835);               // 72 dpi
    put32(hdr + 42, 2835);
    UINT bw = 0;
    bool ok = f_write(&file, hdr, sizeof(hdr), &bw) == FR_OK && bw == sizeof(hdr);

    // Read from the live panel: canvas 0, full-panel Active Window --
    // both restored afterwards (this may run from inside menu drawing)
    const std::uint32_t prevCanvas = d->canvasAddr();
    const std::uint16_t prevStride = d->canvasStride();
    if (prevCanvas != 0) d->endOverlayDraw();
    std::uint16_t awX, awY, awW, awH;
    d->widenActiveWindowForLinearAccess(&awX, &awY, &awW, &awH);

    static std::uint16_t px[SCREEN_MAX_X];
    static std::uint8_t row[rowBytes];
    const absolute_time_t t0 = get_absolute_time();
    for (int y = static_cast<int>(H) - 1; ok && y >= 0; --y) {
        d->readRow565(0, static_cast<std::int16_t>(y), W, px);
        if (withOverlays) {
            overlayPipRow(d, 2, y, px);   // button strip
            overlayPipRow(d, 1, y, px);   // menu etc. -- PIP-1 is drawn on top
        }
        for (std::uint32_t x = 0; x < W; ++x) {
            const std::uint16_t p = px[x];
            const std::uint8_t r = (p >> 11) & 0x1F, g = (p >> 5) & 0x3F, b = p & 0x1F;
            row[3 * x + 0] = static_cast<std::uint8_t>((b << 3) | (b >> 2));
            row[3 * x + 1] = static_cast<std::uint8_t>((g << 2) | (g >> 4));
            row[3 * x + 2] = static_cast<std::uint8_t>((r << 3) | (r >> 2));
        }
        ok = f_write(&file, row, rowBytes, &bw) == FR_OK && bw == rowBytes;
    }

    d->restoreActiveWindow(awX, awY, awW, awH);
    if (prevCanvas != 0) d->beginLayerDraw(prevCanvas, prevStride);

    ok = (f_close(&file) == FR_OK) && ok;
    if (!ok) {
        f_unlink(name);
        message = "Write error";
        return false;
    }
    config.setScreendumpNext(n + 1);
    LOGF("\r\n * Screendump%s saved: %s (%lu ms)", withOverlays ? " (with overlays)" : "", name,
         static_cast<unsigned long>(absolute_time_diff_us(t0, get_absolute_time()) / 1000));
    message = name;
    return true;
}

#else  // 5" panel

bool screendump_save(DisplayDriver*, std::string& message, bool) {
    message = "Not supported on this panel";
    return false;
}

#endif

}  // namespace hipi
