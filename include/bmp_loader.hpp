// bmp_loader.hpp
//
// Loads 24-bit uncompressed BMP files from the SD card and draws them via
// DisplayDriver::drawBitmap565(). Shared by the button-strip loader, the
// splash-screen logo (both in boardui.cpp) and the Tape view image
// (plotterview.cpp) so the BMP-parsing logic only exists once.
#pragma once

#include "display_config.h"
#include "usb_serial.h"  // LOGF
#include "ff.h"
#include <cstdint>
#include <cstring>
#include <vector>
#include <algorithm>
#include "pico/time.h"

namespace hipi {

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

// Decodes a 24-bit uncompressed BMP and draws it at (x0, y0) (unless
// alsoDraw is false, in which case it's only decoded/cached, not drawn --
// e.g. so the button strip's bitmap can be loaded and cached at boot
// without actually appearing on screen until a real tap first shows it).
// Optionally also caches the decoded RGB565 pixels (row-major, stride =
// width) -- e.g. so a button's sub-rectangle can be redrawn later without
// re-reading the file (see ui_buttons.hpp's redrawButtonRegion()).
// The file is read sequentially in ~24 kB chunks (see the loop below), and
// the time spent is logged, split into SD reading and convert + draw.
inline bool drawBmpAt(DisplayDriver* display, const char* path,
                       std::int16_t x0, std::int16_t y0,
                       std::vector<std::uint16_t>* outPixels = nullptr,
                       std::uint16_t* outWidth = nullptr,
                       std::uint16_t* outHeight = nullptr,
                       bool alsoDraw = true) {
    FIL file;
    LOGF("\r\n\t\t* Open file <%s> ... ", path);
    if (f_open(&file, path, FA_READ) != FR_OK) {
        LOGF("\r\n\t * Could not open file!");
        return false;
    }

    std::uint8_t header[54];
    UINT br = 0;
    if (f_read(&file, header, 54, &br) != FR_OK || br != 54 ||
        header[0] != 'B' || header[1] != 'M') {
        LOGF("\r\n\t * No BMP file!");
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

    LOGF("\r\n\t\t* Width: %d height: %d", width, heightRaw);
    LOGF("\r\n\t\t* bpp: %d", bpp);

    // We only support what png_to_bmp24.py generates: uncompressed 24-bit.
    if (bpp != 24 || compression != 0 || width <= 0) {
        LOGF("\r\n\t * Wrong format!");
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

    // Read the pixel data SEQUENTIALLY, several rows per f_read(). A normal
    // (bottom-up) BMP stores the bottom screen row first, so instead of
    // seeking backwards for every row (600 f_lseek() calls for a full
    // 7" image) the file is read front to back and each chunk is drawn
    // from the bottom of the image upwards. Rows are reordered inside the
    // chunk buffer so every chunk is one top-to-bottom drawBitmap565()
    // call. kChunkTargetBytes keeps the two buffers at ~40 kB total.
    constexpr std::uint32_t kChunkTargetBytes = 24 * 1024;
    const std::uint32_t rowsPerChunk =
        std::max<std::uint32_t>(1, std::min<std::uint32_t>(height, kChunkTargetBytes / rowSize));

    std::vector<std::uint8_t>  rawChunk(static_cast<std::size_t>(rowsPerChunk) * rowSize);
    std::vector<std::uint16_t> pixChunk(static_cast<std::size_t>(rowsPerChunk) * width);

    if (f_lseek(&file, dataOffset) != FR_OK) {
        LOGF("\r\n\t * Seek error!");
        f_close(&file);
        return false;
    }

    const absolute_time_t tStart = get_absolute_time();
    std::int64_t readUs = 0;

    for (std::uint32_t fileRow = 0; fileRow < height; fileRow += rowsPerChunk) {
        const std::uint32_t n = std::min<std::uint32_t>(rowsPerChunk, height - fileRow);
        const UINT bytes = static_cast<UINT>(n * rowSize);

        const absolute_time_t tRead = get_absolute_time();
        if (f_read(&file, rawChunk.data(), bytes, &br) != FR_OK || br != bytes) {
            LOGF("\r\n\t * Read error at row %lu", static_cast<unsigned long>(fileRow));
            f_close(&file);
            return false;
        }
        readUs += absolute_time_diff_us(tRead, get_absolute_time());

        // Screen row of the TOP row in this chunk
        const std::uint32_t topScreenRow = topDown ? fileRow : (height - fileRow - n);

        for (std::uint32_t i = 0; i < n; ++i) {
            // i-th row in file order -> its position (top-to-bottom) in the chunk
            const std::uint32_t dstRow = topDown ? i : (n - 1 - i);
            const std::uint8_t* src = rawChunk.data() + static_cast<std::size_t>(i) * rowSize;
            std::uint16_t* dst = pixChunk.data() + static_cast<std::size_t>(dstRow) * width;
            for (std::int32_t col = 0; col < width; ++col) {
                const std::uint8_t b  = src[col * 3 + 0];
                const std::uint8_t g  = src[col * 3 + 1];
                const std::uint8_t rr = src[col * 3 + 2];
                dst[col] = static_cast<std::uint16_t>(
                    ((rr & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
            }
        }

        if (alsoDraw) {
            display->drawBitmap565(x0, static_cast<std::int16_t>(y0 + topScreenRow),
                                   static_cast<std::uint16_t>(width),
                                   static_cast<std::uint16_t>(n),
                                   pixChunk.data());
        }

        if (outPixels) {
            std::memcpy(outPixels->data() + static_cast<std::size_t>(topScreenRow) * width,
                        pixChunk.data(),
                        static_cast<std::size_t>(n) * width * sizeof(std::uint16_t));
        }
    }

    const std::int64_t totalUs = absolute_time_diff_us(tStart, get_absolute_time());
    LOGF("\r\n\t\t* %lu ms (SD read %lu ms, convert + draw %lu ms)",
         static_cast<unsigned long>(totalUs / 1000),
         static_cast<unsigned long>(readUs / 1000),
         static_cast<unsigned long>((totalUs - readUs) / 1000));

    LOGF("\r\n\t\t* Done!");
    f_close(&file);
    return true;
}

}  // namespace hipi