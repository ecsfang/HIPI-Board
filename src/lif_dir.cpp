// lif_dir.cpp -- see lif_dir.h
#include "lif_dir.h"
#include <algorithm>
#include <cstring>

namespace hipi {

namespace {

constexpr std::uint32_t kBlock = 256;

std::uint32_t be(const std::uint8_t* p, int n) {
    std::uint32_t v = 0;
    for (int i = 0; i < n; ++i) v = (v << 8) | p[i];
    return v;
}
void putBe(std::uint8_t* p, int n, std::uint32_t v) {
    for (int i = n - 1; i >= 0; --i) { p[i] = static_cast<std::uint8_t>(v); v >>= 8; }
}
std::string trimmed(const std::uint8_t* p, std::size_t n) {
    std::string s(reinterpret_cast<const char*>(p), n);
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

}  // namespace

std::string lifPaddedName(const std::string& name) {
    std::string s = name.substr(0, kLifNameLen);
    s.resize(kLifNameLen, ' ');
    return s;
}

bool lifFileNameOk(const std::string& name) {
    if (name.empty() || name.size() > kLifNameLen) return false;
    // (printable, no spaces -- and no '|', it separates names in the mirror's
    // messages)
    for (char c : name)
        if (c <= ' ' || c > '~' || c == '|') return false;
    return true;
}

const LifDirEntry* LifDir::find(const std::string& name) const {
    const std::string want = lifPaddedName(name);
    for (const LifDirEntry& e : files)
        if (lifPaddedName(e.name) == want) return &e;
    return nullptr;
}

bool LifDir::findSpace(std::uint32_t blocks, int ignoreIndex, std::uint32_t& start) const {
    // The files' runs, in block order, and an end mark past the medium
    std::vector<std::pair<std::uint32_t, std::uint32_t>> used;
    for (const LifDirEntry& e : files)
        if (e.index != ignoreIndex) used.emplace_back(e.start, e.blocks);
    used.emplace_back(mediumBlocks, 0);
    std::sort(used.begin(), used.end());
    std::uint32_t at = dirStart + dirBlocks;              // the first block after the directory
    for (const auto& u : used) {
        if (u.first >= at && u.first - at >= blocks) { start = at; return true; }
        at = std::max(at, u.first + u.second);
    }
    return false;
}

std::uint32_t LifDir::freeBlocks(int ignoreIndex) const {
    std::uint32_t used = 0;
    for (const LifDirEntry& e : files)
        if (e.index != ignoreIndex) used += e.blocks;
    const std::uint32_t data = mediumBlocks > dirStart + dirBlocks ? mediumBlocks - dirStart - dirBlocks : 0;
    return used >= data ? 0 : data - used;
}

std::uint32_t lifFileLength(const std::uint8_t e[kLifEntrySize]) {
    const std::uint32_t type = be(e + 10, 2);
    const std::uint32_t blocks = be(e + 16, 4) * kBlock;
    const std::uint32_t regs = be(e + 28, 2);                                   // HP-41: registers
    const std::uint32_t nibbles = e[28] | (e[29] << 8) | (static_cast<std::uint32_t>(e[30]) << 16);   // HP-71
    std::uint32_t len = blocks;
    switch (type) {
    case 0xE080: len = be(e + 28, 2) + 1; break;                               // PGM41: + checksum
    case 0xE020: case 0xE030: case 0xE040: case 0xE050: case 0xE060: case 0xE070:
        len = regs * 8 + 1; break;                                              // HP-41 register files
    case 0xE0D0: len = regs * 8; break;                                         // SDATA
    case 0xE0F0: case 0xE0F1:                                                   // DAT71: records x length
        len = (e[28] | (e[29] << 8)) * static_cast<std::uint32_t>(e[30] | (e[31] << 8)); break;
    case 0x00FF: case 0xE204: case 0xE205: case 0xE206: case 0xE207: case 0xE208: case 0xE209:
    case 0xE20A: case 0xE20B: case 0xE20C: case 0xE20D: case 0xE214: case 0xE215: case 0xE216:
    case 0xE217: case 0xE21C: case 0xE222: case 0xE224: case 0xE22E:
        len = (nibbles + 1) / 2; break;                                         // HP-71: nibbles
    default: break;                                                             // whole blocks
    }
    return len == 0 || len > blocks ? blocks : len;
}

bool lifReadDir(const LifReadBlock& read, std::uint32_t fileBlocks, LifDir& dir, std::string& why) {
    dir = LifDir();
    std::uint8_t b[kBlock];
    if (!read(0, b)) { why = "can't read the cassette"; return false; }
    if (be(b, 2) != 0x8000) { why = "not a LIF volume"; return false; }
    dir.label = trimmed(b + 2, 6);
    dir.dirStart = be(b + 8, 4);
    dir.dirBlocks = be(b + 16, 4);
    // The medium: tracks x surfaces x blocks per track, as the header says.
    // An HP-41 (NEWM) leaves them 0 -- a cassette is a cassette for it --
    // and so does an old or odd header (lifput doesn't trust them when all
    // three are the same): then it's a cassette, 512 blocks, as the drive
    // takes it (CDrive::mediaBytes()). The file itself is often shorter:
    // it ends after the last block written (the drive reads 0xFF past the
    // end, and writing there makes the file longer), so its size is no
    // limit -- only a bigger file (a disk image without a size) counts
    const std::uint32_t t = be(b + 24, 4), s = be(b + 28, 4), n = be(b + 32, 4);
    const std::uint64_t geometry = static_cast<std::uint64_t>(t) * s * n;
    std::uint32_t medium = (t == s && s == n) || geometry == 0 || geometry > (1u << 24)
                               ? std::max(fileBlocks, kLifCassetteBlocks) : static_cast<std::uint32_t>(geometry);
    dir.mediumBlocks = medium;
    if (dir.dirStart < 1 || dir.dirBlocks < 1 || dir.dirStart + dir.dirBlocks > medium) {
        why = "the cassette's directory is damaged";
        return false;
    }
    for (std::uint32_t blk = 0; blk < dir.dirBlocks; ++blk) {
        if (!read(dir.dirStart + blk, b)) { why = "can't read the cassette's directory"; return false; }
        for (int k = 0; k < 8; ++k) {
            const std::uint8_t* e = b + k * kLifEntrySize;
            const int index = static_cast<int>(blk * 8 + static_cast<std::uint32_t>(k));
            const std::uint16_t type = static_cast<std::uint16_t>(be(e + 10, 2));
            if (type == 0xFFFF) {                         // the end mark
                if (dir.firstFree < 0) { dir.firstFree = index; dir.freeIsEnd = true; }
                return true;
            }
            if (type == 0) {                              // deleted: its place is free
                if (dir.firstFree < 0) dir.firstFree = index;
                continue;
            }
            LifDirEntry f;
            f.index = index;
            f.name = trimmed(e, kLifNameLen);
            f.type = type;
            f.start = be(e + 12, 4);
            f.blocks = be(e + 16, 4);
            std::memcpy(f.entry, e, kLifEntrySize);
            dir.files.push_back(f);
        }
    }
    return true;                                          // (full: no end mark)
}

bool lifAddFile(const LifReadBlock& read, const LifWriteBlock& write, std::uint32_t fileBlocks,
                const std::uint8_t entry[kLifEntrySize], const LifReadBlock& data, bool replace,
                std::string& why) {
    LifDir dir;
    if (!lifReadDir(read, fileBlocks, dir, why)) return false;
    const std::string name = trimmed(entry, kLifNameLen);
    const std::uint32_t blocks = be(entry + 16, 4);
    const LifDirEntry* old = dir.find(name);
    if (old != nullptr && !replace) { why = name + " is on the cassette already"; return false; }
    const int oldIndex = old != nullptr ? old->index : -1;
    // Where the entry goes: the old one's place, else the first free one
    int slot = oldIndex;
    bool slotIsEnd = false;
    if (slot < 0) {
        if (dir.firstFree < 0) { why = "the cassette's directory is full"; return false; }
        slot = dir.firstFree;
        slotIsEnd = dir.freeIsEnd;
    }
    std::uint32_t start = 0;
    if (!dir.findSpace(blocks, oldIndex, start)) {
        why = "no room on the cassette (" + std::to_string(blocks) + (blocks == 1 ? " block" : " blocks") + " needed, " +
              std::to_string(dir.freeBlocks(oldIndex)) + " free" +
              (dir.freeBlocks(oldIndex) >= blocks ? ", but not in one piece" : "") + ")";
        return false;
    }
    // The data first, then the directory: a failure on the way leaves the
    // directory as it was. A file shorter than where the data goes: zeros up
    // to there first (free blocks, but the file shouldn't have a gap of
    // whatever the SD card had there)
    std::uint8_t b[kBlock];
    if (fileBlocks != 0 && fileBlocks < start) {
        std::memset(b, 0, sizeof b);
        for (std::uint32_t i = fileBlocks; i < start; ++i)
            if (!write(i, b)) { why = "writing to the cassette failed"; return false; }
    }
    for (std::uint32_t i = 0; i < blocks; ++i) {
        if (!data(i, b)) { why = "can't read the file"; return false; }
        if (!write(start + i, b)) { why = "writing to the cassette failed"; return false; }
    }
    auto writeEntry = [&](int index, const std::uint8_t* e) {
        const std::uint32_t blk = dir.dirStart + static_cast<std::uint32_t>(index) / 8;
        std::uint8_t d[kBlock];
        if (!read(blk, d)) return false;
        std::memcpy(d + (index % 8) * kLifEntrySize, e, kLifEntrySize);
        return write(blk, d);
    };
    std::uint8_t e[kLifEntrySize];
    std::memcpy(e, entry, kLifEntrySize);
    putBe(e + 12, 4, start);
    if (!writeEntry(slot, e)) { why = "writing the cassette's directory failed"; return false; }
    // The end mark moves one on (unless the new entry took the last place)
    if (slotIsEnd && slot + 1 < dir.capacity()) {
        std::uint8_t endMark[kLifEntrySize] = {};
        endMark[10] = endMark[11] = 0xFF;
        if (!writeEntry(slot + 1, endMark)) { why = "writing the cassette's directory failed"; return false; }
    }
    return true;
}

}  // namespace hipi
