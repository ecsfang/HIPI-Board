// lif_info.hpp -- builds a human-readable description of a LIF volume
// (the .dat cassette images): the volume header and the directory, one
// text line each. Format per "LIF: The Logical Interchange Format",
// https://www.finseth.com/hpdata/lif.php -- 256-byte blocks, big-endian,
// header in block 0, directory (32-byte entries) from block 2.
#pragma once
#include "ff.h"
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace hipi {

namespace lifdetail {

inline std::uint32_t be16(const std::uint8_t* p) { return (p[0] << 8) | p[1]; }
inline std::uint32_t be32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}
inline int bcd(std::uint8_t b) { return (b >> 4) * 10 + (b & 0x0F); }

// Fixed-length, space-padded text field -> trimmed string
inline std::string field(const std::uint8_t* p, int len) {
    std::string s;
    for (int i = 0; i < len; ++i) s += (p[i] >= 32 && p[i] < 127) ? static_cast<char>(p[i]) : '?';
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

// 6-byte BCD date/time (year-1900, month, day, hour, min, sec), "" if unset
inline std::string dateTime(const std::uint8_t* p) {
    bool any = false;
    for (int i = 0; i < 6; ++i) any = any || p[i] != 0;
    if (!any) return "";
    const int y = bcd(p[0]);
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d",
                  y < 70 ? 2000 + y : 1900 + y, bcd(p[1]), bcd(p[2]), bcd(p[3]), bcd(p[4]));
    return buf;
}

// Short type name, as the HP-41 / HP-71 show them
inline const char* typeName(std::uint32_t t) {
    switch (t) {
        case 0x0001: case 0xE0D5: return "AS";    // LIF text / -41 ASCII
        case 0xE0D0: return "DA";                 // data
        case 0xE080: return "PR";                 // FOCAL program
        case 0xE040: return "WA";                 // write all
        case 0xE050: return "KE";                 // key assignments
        case 0xE060: return "ST";                 // status
        case 0xE070: return "ROM";
        case 0xE020: return "CA";                 // whole calculator (HOSER)
        case 0xE030: return "XM";                 // extended memory (HOSER)
        case 0xE0B0: return "BU";                 // buffer (HOSER)
        case 0xE214: case 0xE215: case 0xE216: case 0xE217: return "BAS";
        case 0xE204: case 0xE205: case 0xE206: case 0xE207: return "BIN";
        case 0xE208: case 0xE209: case 0xE20A: case 0xE20B: return "LEX";
        case 0xE20C: case 0xE20D: return "KEY";
        case 0xE0F0: case 0xE0F1: return "DAT";
        default: return nullptr;
    }
}

// Size column: HP-41 files in registers ("35r", as the -41's DIR shows
// them), HP-71 files in bytes ("512b"), otherwise "-"
inline std::string sizeText(std::uint32_t t, const std::uint8_t* misc) {
    char buf[24];
    const std::uint32_t m23 = be16(misc + 2);
    if (t == 0xE080) {                                   // bytes - 1
        std::snprintf(buf, sizeof(buf), "%ur", static_cast<unsigned>((m23 + 1 + 6) / 7));
    } else if (t == 0xE0D0) {                            // registers
        std::snprintf(buf, sizeof(buf), "%ur", static_cast<unsigned>(m23));
    } else if (t >= 0xE020 && t <= 0xE0B0 && (t & 0x0F) == 0) {   // -41: registers - 1
        std::snprintf(buf, sizeof(buf), "%ur", static_cast<unsigned>(m23 + 1));
    } else if ((t >= 0xE204 && t <= 0xE217) || t == 0xE0F0 || t == 0xE0F1) {
        if (t == 0xE0F0 || t == 0xE0F1) {                // records x record length
            std::snprintf(buf, sizeof(buf), "%lub",
                          static_cast<unsigned long>(m23) * be16(misc + 4));
        } else {                                         // nibbles
            const std::uint32_t nib = (static_cast<std::uint32_t>(misc[2]) << 16) | (misc[3] << 8) | misc[4];
            std::snprintf(buf, sizeof(buf), "%lub", static_cast<unsigned long>((nib + 1) / 2));
        }
    } else {
        return "-";
    }
    return buf;
}

}  // namespace lifdetail

// Fills `lines` with the description of the LIF volume in file `path`.
// Returns false (with an explanation in `lines`) if it can't be read or
// isn't LIF.
inline bool lifDescribe(const char* path, std::vector<std::string>& lines) {
    using namespace lifdetail;
    lines.clear();
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) {
        lines.push_back("Can't open the file");
        return false;
    }
    std::uint8_t blk[256];
    UINT br = 0;
    if (f_read(&f, blk, 256, &br) != FR_OK || br != 256 || be16(blk) != 0x8000) {
        f_close(&f);
        lines.push_back("Not a LIF volume");
        return false;
    }
    const std::uint32_t mediaBlocks = static_cast<std::uint32_t>(f_size(&f) / 256);
    const std::uint32_t dirStart = be32(blk + 8);
    std::uint32_t dirLen = be32(blk + 16);
    char buf[80];

    const std::string label = field(blk + 2, 6);
    lines.push_back("Volume : " + (label.empty() ? std::string("(no label)") : label));
    std::snprintf(buf, sizeof(buf), "Media  : %lu blocks (%lu bytes)",
                  static_cast<unsigned long>(mediaBlocks), static_cast<unsigned long>(mediaBlocks) * 256);
    lines.push_back(buf);
    const std::string init = dateTime(blk + 36);
    if (!init.empty()) lines.push_back("Created: " + init);
    std::snprintf(buf, sizeof(buf), "Dir    : block %lu, %lu blocks, %lu entries",
                  static_cast<unsigned long>(dirStart), static_cast<unsigned long>(dirLen),
                  static_cast<unsigned long>(dirLen * 8));
    lines.push_back(buf);
    const std::size_t summaryAt = lines.size();          // filled in below
    lines.push_back("");
    lines.push_back("");
    lines.push_back("NAME       TYPE    SIZE  BLOCKS  FLAGS");

    if (dirLen > 64) dirLen = 64;                        // sanity cap: 512 entries
    unsigned files = 0;
    std::uint32_t usedBlocks = 0;
    bool done = f_lseek(&f, static_cast<FSIZE_t>(dirStart) * 256) != FR_OK;
    for (std::uint32_t b = 0; b < dirLen && !done; ++b) {
        if (f_read(&f, blk, 256, &br) != FR_OK || br != 256) break;
        for (int e = 0; e < 8; ++e) {
            const std::uint8_t* d = blk + e * 32;
            const std::uint32_t type = be16(d + 10);
            if (type == 0xFFFF) { done = true; break; }  // end of directory
            if (type == 0x0000) continue;                // purged
            const std::uint32_t alloc = be32(d + 16);
            const std::uint8_t* misc = d + 26;
            const char* tn = typeName(type);
            char tbuf[12];
            if (tn == nullptr) { std::snprintf(tbuf, sizeof(tbuf), "%04lX", static_cast<unsigned long>(type)); tn = tbuf; }
            std::string flags;
            if (type >= 0xE020 && type <= 0xE0D0) {      // -41 style secure/private bits
                if (misc[4] & 0x80) flags += 'S';
                if (misc[4] & 0x01) flags += 'P';
            }
            std::snprintf(buf, sizeof(buf), "%-10s %-4s %7s %7lu  %s",
                          field(d, 10).c_str(), tn, sizeText(type, misc).c_str(),
                          static_cast<unsigned long>(alloc), flags.c_str());
            lines.push_back(buf);
            ++files;
            usedBlocks += alloc;
        }
    }
    f_close(&f);

    const std::uint32_t overhead = dirStart + dirLen;
    const std::uint32_t freeBlocks = mediaBlocks > overhead + usedBlocks
                                     ? mediaBlocks - overhead - usedBlocks : 0;
    std::snprintf(buf, sizeof(buf), "Files  : %u, %lu blocks used, %lu free", files,
                  static_cast<unsigned long>(usedBlocks), static_cast<unsigned long>(freeBlocks));
    lines[summaryAt] = buf;
    if (files == 0) lines.push_back("(no files)");
    return true;
}

}  // namespace hipi
