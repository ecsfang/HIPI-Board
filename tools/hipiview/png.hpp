// png.hpp -- writes a 0xAARRGGBB image as an RGB PNG file (zlib).
#pragma once
#include <zlib.h>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

inline bool writePng(const std::string& path, const std::uint32_t* px, int w, int h) {
    std::vector<std::uint8_t> raw;
    raw.reserve(static_cast<std::size_t>(h) * (1 + 3 * w));
    for (int y = 0; y < h; ++y) {
        raw.push_back(0);                               // filter: none
        for (int x = 0; x < w; ++x) {
            const std::uint32_t c = px[static_cast<std::size_t>(y) * w + x];
            raw.push_back(static_cast<std::uint8_t>(c >> 16));
            raw.push_back(static_cast<std::uint8_t>(c >> 8));
            raw.push_back(static_cast<std::uint8_t>(c));
        }
    }
    uLongf zlen = compressBound(static_cast<uLong>(raw.size()));
    std::vector<std::uint8_t> z(zlen);
    if (compress2(z.data(), &zlen, raw.data(), static_cast<uLong>(raw.size()), 6) != Z_OK) return false;
    z.resize(zlen);

    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    auto be32 = [](std::vector<std::uint8_t>& v, std::uint32_t x) {
        v.push_back(static_cast<std::uint8_t>(x >> 24)); v.push_back(static_cast<std::uint8_t>(x >> 16));
        v.push_back(static_cast<std::uint8_t>(x >> 8));  v.push_back(static_cast<std::uint8_t>(x));
    };
    auto chunk = [&](const char* type, const std::vector<std::uint8_t>& data) {
        std::vector<std::uint8_t> c;
        be32(c, static_cast<std::uint32_t>(data.size()));
        c.insert(c.end(), type, type + 4);
        c.insert(c.end(), data.begin(), data.end());
        const std::uint32_t crc = static_cast<std::uint32_t>(
            crc32(0, c.data() + 4, static_cast<uInt>(c.size() - 4)));
        be32(c, crc);
        std::fwrite(c.data(), 1, c.size(), f);
    };
    static const std::uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    std::fwrite(sig, 1, 8, f);
    std::vector<std::uint8_t> ihdr;
    be32(ihdr, static_cast<std::uint32_t>(w));
    be32(ihdr, static_cast<std::uint32_t>(h));
    ihdr.push_back(8); ihdr.push_back(2); ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);
    chunk("IHDR", ihdr);
    chunk("IDAT", z);
    chunk("IEND", {});
    return std::fclose(f) == 0;
}
