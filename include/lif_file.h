// lif_file.h -- a file from the PC as a LIF file: its 32-byte directory
// entry and its data blocks, ready to be added to a cassette (lif_dir.h).
// What kind of file it is goes by its extension; each kind has its own
// conversion. Shared by hipiview (a file dropped on the Tape view is
// converted there, upload.hpp) and the tests; plain C++17.
//
// Kinds so far:
//   .raw   an HP-41 program as in its memory (V41, lifutils' lifget -r ...):
//          a PGM41 file (type 0xE080), as lifutils' raw41lif makes it
//   a LIF file with its directory entry first (lifutils' lifget without
//          -r, HIPI's Save on PC of anything but a program: NAME.lif) --
//          by its content, any extension: lifIsSingleFile()
// More go into kLifFileKinds (a text file, SDATA, a write-all ...).
#pragma once
#include "lif_types.h"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

namespace hipi {

struct LifFileImage {
    std::string name;                    // the LIF name (no padding)
    std::uint16_t type = 0;
    const char* what = "";               // "HP-41 program"
    std::uint32_t length = 0;            // the data's length in bytes (before padding)
    std::vector<std::uint8_t> bytes;     // the entry (32 bytes), then the data blocks
    std::uint32_t blocks() const { return bytes.size() < 32 ? 0 : static_cast<std::uint32_t>((bytes.size() - 32) / 256); }
};

// The LIF name from the PC's file name: its name without the extension,
// upper case, letters and digits only, at most 10 -- a letter first ("P"
// put in front if it starts with a digit). "" if nothing is left.
inline std::string lifNameFromPcName(const std::string& pcName) {
    std::string base = pcName;
    const std::size_t slash = base.find_last_of("/\\");
    if (slash != std::string::npos) base = base.substr(slash + 1);
    const std::size_t dot = base.find_last_of('.');
    if (dot != std::string::npos && dot > 0) base.resize(dot);
    std::string n;
    for (char c : base) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (std::isalnum(u) && u < 0x80) n += static_cast<char>(std::toupper(u));
    }
    if (!n.empty() && std::isdigit(static_cast<unsigned char>(n[0]))) n = "P" + n;
    if (n.size() > 10) n.resize(10);
    return n;
}

namespace lif_file_detail {

inline void putBe(std::uint8_t* p, int n, std::uint32_t v) {
    for (int i = n - 1; i >= 0; --i) { p[i] = static_cast<std::uint8_t>(v); v >>= 8; }
}

// The entry and the data, padded to whole blocks
inline void build(LifFileImage& f, const std::uint8_t typeBytes[4], const std::vector<std::uint8_t>& data) {
    const std::uint32_t blocks = static_cast<std::uint32_t>((data.size() + 255) / 256);
    f.length = static_cast<std::uint32_t>(data.size());
    f.bytes.assign(32 + static_cast<std::size_t>(blocks) * 256, 0);
    std::uint8_t* e = f.bytes.data();
    for (std::size_t i = 0; i < 10; ++i) e[i] = i < f.name.size() ? static_cast<std::uint8_t>(f.name[i]) : ' ';
    putBe(e + 10, 2, f.type);
    putBe(e + 12, 4, 0);                           // (the start: where it goes)
    putBe(e + 16, 4, blocks);
    // 20-25 the date: none (as lifutils for HP-41 files)
    e[26] = 0x80;                                  // the last volume, number 1
    e[27] = 0x01;
    for (int i = 0; i < 4; ++i) e[28 + i] = typeBytes[i];
    std::copy(data.begin(), data.end(), f.bytes.begin() + 32);
}

// ── HP-41 program (raw) ─────────────────────────────────────────────────
// How long the program is: up to and including its END (lifutils'
// raw41lif_decode_prog). The instruction lengths from the first byte:
// 'Extend Your HP41' section 15.7
inline std::size_t hp41ProgramLength(const std::vector<std::uint8_t>& m, bool& foundEnd) {
    auto at = [&](std::size_t i) -> std::uint8_t { return i < m.size() ? m[i] : 0; };
    std::size_t pc = 0;
    foundEnd = false;
    while (pc < m.size() && !foundEnd) {
        const std::uint8_t op = m[pc];
        switch (op >> 4) {
        case 0x0: pc += 1; break;                                   // short LBL
        case 0x1:
            if (op >= 0x1D) pc += (at(pc + 1) >> 4) == 0xF ? (at(pc + 1) & 0xF) + 2u : 2u;   // GTO/XEQ "alpha"
            else do { ++pc; } while (pc < m.size() && m[pc] >= 0x10 && m[pc] <= 0x1C);       // digits
            break;
        case 0x2: case 0x3: pc += 1; break;                         // short STO / RCL
        case 0x4: case 0x5: case 0x6: case 0x7: case 0x8: pc += 1; break;
        case 0x9: case 0xA: pc += 2; break;
        case 0xB: pc += 2; break;                                   // short GTO
        case 0xC:
            if (op >= 0xCE) { pc += 2; break; }                     // X<> / LBL nn
            // A global label (a key assignment after it) or an END
            if (at(pc + 2) & 0x80) pc += (at(pc + 2) & 0xF) + 3u;
            else { pc += 3; foundEnd = true; }
            break;
        case 0xD: case 0xE: pc += 3; break;                         // GTO / XEQ nn
        case 0xF: pc += (op & 0xF) + 1u; break;                     // text
        }
    }
    return std::min(pc, m.size());
}

inline bool hp41Raw(const std::vector<std::uint8_t>& raw, LifFileImage& f, std::string& why) {
    bool foundEnd = false;
    const std::size_t len = hp41ProgramLength(raw, foundEnd);
    if (raw.empty() || !foundEnd) { why = "not an HP-41 program (no END in it)"; return false; }
    if (len > 0xFFFF) { why = "too long for an HP-41 program"; return false; }
    // The program, then its checksum (the bytes added up, the low 8 bits)
    std::vector<std::uint8_t> data(raw.begin(), raw.begin() + static_cast<std::ptrdiff_t>(len));
    std::uint32_t sum = 0;
    for (std::uint8_t b : data) sum += b;
    data.push_back(static_cast<std::uint8_t>(sum));
    f.type = 0xE080;                                                // PGM41
    f.what = "HP-41 program";
    // 28-29 the program's length in bytes, 30-31 0x0020 (as raw41lif)
    const std::uint8_t typeBytes[4] = { static_cast<std::uint8_t>(len >> 8), static_cast<std::uint8_t>(len), 0x00, 0x20 };
    build(f, typeBytes, data);
    return true;
}

}  // namespace lif_file_detail

struct LifFileKind {
    const char* ext;                     // ".raw" (any case)
    const char* what;                    // "HP-41 program"
    bool (*convert)(const std::vector<std::uint8_t>& in, LifFileImage& out, std::string& why);
};

inline const LifFileKind kLifFileKinds[] = {
    { ".raw", "HP-41 program", lif_file_detail::hp41Raw },
};

// The kind of a file by its name's extension; nullptr: none (not a file
// that can be added to a cassette)
inline const LifFileKind* lifFileKind(const std::string& pcName) {
    std::string l = pcName;
    for (char& c : l) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (const LifFileKind& k : kLifFileKinds) {
        const std::string ext = k.ext;
        if (l.size() > ext.size() && l.compare(l.size() - ext.size(), ext.size(), ext) == 0) return &k;
    }
    return nullptr;
}

// A LIF file as lifutils' `lifget` writes it -- and lifput, or a Save on PC
// of HIPI's Media map, gives it (NAME.lif): its 32-byte directory entry,
// then its data (as long as its type says, up to its blocks). Not a whole
// volume (that starts 0x8000). Its name and type are the entry's.
inline bool lifIsSingleFile(const std::vector<std::uint8_t>& in) {
    if (in.size() < 32 + 1 || (in[0] == 0x80 && in[1] == 0x00)) return false;
    if (in[0] <= ' ' || in[0] > '~') return false;           // (a name: the HP-41's may start with a digit)
    for (int i = 1; i < 10; ++i)
        if (in[i] < 0x20 || in[i] > 0x7E) return false;
    const std::uint32_t type = (in[10] << 8) | in[11];
    const std::uint32_t blocks = (static_cast<std::uint32_t>(in[16]) << 24) | (in[17] << 16) | (in[18] << 8) | in[19];
    const std::uint64_t len = in.size() - 32;
    // (a sane entry: "last volume, number 1" as LIF writes it, its length
    // within its last block, at most a megabyte)
    return type != 0 && type != 0xFFFF && in[26] == 0x80 && in[27] == 0x01 && blocks > 0 && blocks <= 4096 &&
           len <= static_cast<std::uint64_t>(blocks) * 256 && len >= static_cast<std::uint64_t>(blocks - 1) * 256;
}
inline bool lifFileFromLifget(const std::vector<std::uint8_t>& in, LifFileImage& out, std::string& why) {
    if (!lifIsSingleFile(in)) { why = "not a LIF file (no directory entry at its start)"; return false; }
    out = LifFileImage();
    out.name.assign(reinterpret_cast<const char*>(in.data()), 10);
    while (!out.name.empty() && out.name.back() == ' ') out.name.pop_back();
    out.type = static_cast<std::uint16_t>((in[10] << 8) | in[11]);
    const LifType* t = lifType(out.type);
    out.what = t != nullptr ? t->what : "LIF file";
    const std::uint32_t blocks = (static_cast<std::uint32_t>(in[16]) << 24) | (in[17] << 16) | (in[18] << 8) | in[19];
    out.length = static_cast<std::uint32_t>(in.size() - 32);
    out.bytes.assign(32 + static_cast<std::size_t>(blocks) * 256, 0);
    std::copy(in.begin(), in.end(), out.bytes.begin());
    for (int i = 12; i < 16; ++i) out.bytes[static_cast<std::size_t>(i)] = 0;   // (the start: where it goes)
    return true;
}

// The file from the PC as a LIF file, named after it (lifNameFromPcName)
inline bool lifFileFromPc(const std::string& pcName, const std::vector<std::uint8_t>& in, LifFileImage& out,
                          std::string& why) {
    const LifFileKind* k = lifFileKind(pcName);
    if (k == nullptr) { why = "not a file HIPI can put on a cassette"; return false; }
    out = LifFileImage();
    out.name = lifNameFromPcName(pcName);
    if (out.name.empty()) { why = "no name for it on the cassette (letters and digits)"; return false; }
    if (!k->convert(in, out, why)) return false;
    out.what = k->what;
    return true;
}

}  // namespace hipi
