// bmp_loader.hpp
//
// Loads 24-bit uncompressed BMP files from the SD card and draws them via
// RA8875::drawBitmap565(). Shared by the button-strip loader (pico_main.cpp)
// and the splash-screen logo (uidialog.hpp) so the BMP-parsing logic only
// exists once.
#pragma once

#include "RA8875.hpp"
#include "ff.h"
#include <cstdint>
#include <cstring>
#include <vector>

namespace hp82163 {

// Quick peek at a BMP's dimensions (opens, reads the 54-byte header, closes)
// without decoding any pixel data. Useful when you need the width/height
// before deciding where to draw it (e.g. right-alignment).
inline bool peekBmpDimensions(const char* path, std::uint16_t& outWidth, std::uint16_t& outHeight) {
    FIL file;
    if (f_open(&file, path, FA_READ) != FR_OK) return false;

    std::uint8_t header[54];
    UINT br = 0;
    const bool ok = (f_read(&file, header, 54, &br) == FR_OK && br == 54 &&
                     header[0] == 'B' && header[1] == 'M');
    f_close(&file);
    if (!ok) return false;

    auto rd32 = [&](int off) -> std::int32_t {
        return static_cast<std::int32_t>(
            header[off] | (header[off+1] << 8) |
            (header[off+2] << 16) | (header[off+3] << 24));
    };
    const std::int32_t width     = rd32(18);
    const std::int32_t heightRaw = rd32(22);
    if (width <= 0) return false;

    outWidth  = static_cast<std::uint16_t>(width);
    outHeight = static_cast<std::uint16_t>(heightRaw < 0 ? -heightRaw : heightRaw);
    return true;
}

// Decodes a 24-bit uncompressed BMP and draws it at (x0, y0). Optionally
// also caches the decoded RGB565 pixels (row-major, stride = width) --
// e.g. so a button's sub-rectangle can be redrawn later without re-reading
// the file (see ui_buttons.hpp's redrawButtonRegion()).
inline bool drawBmpAt(RA8875& display, const char* path,
                       std::int16_t x0, std::int16_t y0,
                       std::vector<std::uint16_t>* outPixels = nullptr,
                       std::uint16_t* outWidth = nullptr,
                       std::uint16_t* outHeight = nullptr) {
    FIL file;
    cdc0_printf("\r\n\t * Open file <%s> ... ", path);
    if (f_open(&file, path, FA_READ) != FR_OK) {
        cdc0_printf("\r\n\t * Could not open file!");
        return false;
    }

    std::uint8_t header[54];
    UINT br = 0;
    if (f_read(&file, header, 54, &br) != FR_OK || br != 54 ||
        header[0] != 'B' || header[1] != 'M') {
        cdc0_printf("\r\n\t * No BMP file!");
        f_close(&file);
        return false;
    }

    auto rd32 = [&](int off) -> std::int32_t {
        return static_cast<std::int32_t>(
            header[off] | (header[off+1] << 8) |
            (header[off+2] << 16) | (header[off+3] << 24));
    };
    auto rd16 = [&](int off) -> std::int16_t {
        return static_cast<std::int16_t>(header[off] | (header[off+1] << 8));
    };

    const std::uint32_t dataOffset  = static_cast<std::uint32_t>(rd32(10));
    const std::int32_t  width       = rd32(18);
    const std::int32_t  heightRaw   = rd32(22);
    const std::int16_t  bpp         = rd16(28);
    const std::int32_t  compression = rd32(30);

    cdc0_printf("\r\n\t * Width: %d height: %d", width, heightRaw);
    cdc0_printf("\r\n\t * bpp: %d", bpp);

    // We only support what png_to_bmp24.py generates: uncompressed 24-bit.
    if (bpp != 24 || compression != 0 || width <= 0) {
        cdc0_printf("\r\n\t * Wrong format!");
        f_close(&file);
        return false;
    }

    const bool topDown = heightRaw < 0;
    const std::uint32_t height  = static_cast<std::uint32_t>(topDown ? -heightRaw : heightRaw);
    const std::uint32_t rowSize = ((static_cast<std::uint32_t>(width) * 3 + 3) / 4) * 4;  // 4-byte padding

    if (outPixels) {
        outPixels->assign(static_cast<std::size_t>(width) * height, 0);
    }
    if (outWidth)  *outWidth  = static_cast<std::uint16_t>(width);
    if (outHeight) *outHeight = static_cast<std::uint16_t>(height);

    std::vector<std::uint8_t>  rawRow(rowSize);
    std::vector<std::uint16_t> rowBuf(static_cast<std::size_t>(width));

    for (std::uint32_t r = 0; r < height; ++r) {
        // BMPs are normally stored bottom-up when height is positive.
        const std::uint32_t fileRow = topDown ? r : (height - 1 - r);

        f_lseek(&file, dataOffset + static_cast<FSIZE_t>(fileRow) * rowSize);
        if (f_read(&file, rawRow.data(), rowSize, &br) != FR_OK || br != rowSize) {
            cdc0_printf("\r\n\t * Error ... ?");
            f_close(&file);
            return false;
        }

        for (std::int32_t col = 0; col < width; ++col) {
            const std::uint8_t b  = rawRow[static_cast<std::size_t>(col) * 3 + 0];
            const std::uint8_t g  = rawRow[static_cast<std::size_t>(col) * 3 + 1];
            const std::uint8_t rr = rawRow[static_cast<std::size_t>(col) * 3 + 2];
            // RA8875 16bpp format: RGB565
            rowBuf[static_cast<std::size_t>(col)] = static_cast<std::uint16_t>(
                ((rr & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
        }

        display.drawBitmap565(x0, static_cast<std::int16_t>(y0 + r),
                              static_cast<std::uint16_t>(width), 1,
                              rowBuf.data());

        if (outPixels) {
            std::memcpy(outPixels->data() + static_cast<std::size_t>(r) * width,
                        rowBuf.data(), static_cast<std::size_t>(width) * sizeof(std::uint16_t));
        }
    }

    cdc0_printf("\r\n\t * Done!");
    f_close(&file);
    return true;
}

}  // namespace hp82163
