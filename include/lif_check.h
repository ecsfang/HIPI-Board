// lif_check.h -- is this a LIF volume (a cassette / disk image, .dat /
// .lif)? From its first block and its size: the LIF id 0x8000, the
// directory where the header says, inside the file. Shared by the firmware
// (mirror_upload.cpp: a file sent from hipiview) and tools/hipiview (the
// file dropped on the window), so plain C++17, no FatFs.
#pragma once
#include <cstddef>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>

namespace hipi {

constexpr std::uint32_t kLifBlock = 256;

// block0: the file's first 256 bytes (or fewer, if it's shorter: then it
// isn't one). why: what's wrong, for a message.
inline bool lifCheckVolume(const std::uint8_t* block0, std::size_t have, std::uint64_t size, std::string& why) {
    auto be16 = [](const std::uint8_t* p) { return static_cast<std::uint32_t>((p[0] << 8) | p[1]); };
    auto be32 = [](const std::uint8_t* p) {
        return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
               (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
    };
    if (size < 2 * kLifBlock || have < 24) { why = "too short for a LIF volume"; return false; }
    if (be16(block0) != 0x8000) { why = "not a LIF volume (no LIF id at the start)"; return false; }
    const std::uint32_t dirStart = be32(block0 + 8);
    const std::uint32_t dirLen = be32(block0 + 16);
    if (dirStart < 1 || dirLen < 1) { why = "LIF header damaged (no directory)"; return false; }
    if ((static_cast<std::uint64_t>(dirStart) + dirLen) * kLifBlock > size) {
        why = "LIF directory beyond the end of the file";
        return false;
    }
    why.clear();
    return true;
}

// The largest volume taken (a 9114 disk is 630 kB, an HDRIVE16 16 MB)
constexpr std::uint64_t kLifLargestVolume = 32ull << 20;

// A name for a volume on the SD card (lif/<name>): .dat / .lif, at most 48
// characters: letters, digits, space and - _ . ( ) + #
inline bool lifVolumeNameOk(const std::string& name, std::string& why) {
    if (name.empty() || name.size() > 48) { why = "the file name is too long"; return false; }
    for (char c : name) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (!(std::isalnum(u) || std::strchr(" -_.()+#", c) != nullptr)) {
            why = "the file name has characters HIPI can't use";
            return false;
        }
    }
    std::string l = name;
    for (char& c : l) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (name[0] == '.' || l.size() < 5 ||
        (l.compare(l.size() - 4, 4, ".dat") != 0 && l.compare(l.size() - 4, 4, ".lif") != 0)) {
        why = "only .dat and .lif files (LIF volumes)";
        return false;
    }
    return true;
}

}  // namespace hipi
