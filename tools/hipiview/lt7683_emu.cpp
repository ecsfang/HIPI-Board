// lt7683_emu.cpp -- see lt7683_emu.hpp. Register numbers and bit meanings
// follow the LT768x datasheet as used by HIPI's own driver
// (include/LT7683.hpp, src/LT7683.cpp).
#include "lt7683_emu.hpp"
#include "../../include/mirror_protocol.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace mp = hipi::mirror;

namespace {
constexpr std::size_t kSdramSize = 16u * 1024u * 1024u;   // LT7683: 128 Mbit
constexpr std::uint8_t MRWDP = 0x04;

std::int16_t s16(std::uint16_t v) { return static_cast<std::int16_t>(v); }

// BTE raster operation on source 0 and source 1 (datasheet ROP table)
std::uint16_t rop(int code, std::uint16_t s0, std::uint16_t s1) {
    switch (code & 0x0F) {
    case 0x0: return 0x0000;
    case 0x1: return static_cast<std::uint16_t>(~s0 & ~s1);
    case 0x2: return static_cast<std::uint16_t>(~s0 & s1);
    case 0x3: return static_cast<std::uint16_t>(~s0);
    case 0x4: return static_cast<std::uint16_t>(s0 & ~s1);
    case 0x5: return static_cast<std::uint16_t>(~s1);
    case 0x6: return static_cast<std::uint16_t>(s0 ^ s1);
    case 0x7: return static_cast<std::uint16_t>(~s0 | ~s1);
    case 0x8: return static_cast<std::uint16_t>(s0 & s1);
    case 0x9: return static_cast<std::uint16_t>(~(s0 ^ s1));
    case 0xA: return s1;
    case 0xB: return static_cast<std::uint16_t>(~s0 | s1);
    case 0xC: return s0;
    case 0xD: return static_cast<std::uint16_t>(s0 | ~s1);
    case 0xE: return static_cast<std::uint16_t>(s0 | s1);
    default:  return 0xFFFF;
    }
}
}  // namespace

LT7683Emu::LT7683Emu() : mem_(kSdramSize, 0) {
    std::memset(font_, 0, sizeof(font_));
    reset();
}

void LT7683Emu::reset() {
    std::memset(regs_, 0, sizeof(regs_));
    std::memset(pip_, 0, sizeof(pip_));
    cur_ = 0;
    dummy_ = false;
    wrLo_ = rdLo_ = ucgHi_ = -1;
    bteMcu_ = false;
    bteCount_ = 0;
}

void LT7683Emu::setBuiltinFont(const std::uint8_t font[256][16]) {
    std::memcpy(font_, font, sizeof(font_));
    fontKnown_ = true;
}

int LT7683Emu::syncPercent() const {
    if (syncTotal_ == 0) return 0;
    return static_cast<int>(std::min<std::uint64_t>(100, static_cast<std::uint64_t>(syncDone_) * 100 / syncTotal_));
}

// ── Stream parser ───────────────────────────────────────────────────────

void LT7683Emu::feed(const std::uint8_t* data, std::size_t n) {
    pending_.insert(pending_.end(), data, data + n);
    std::size_t pos = 0;
    while (pos < pending_.size()) {
        const std::size_t used = parse(pending_.data() + pos, pending_.size() - pos);
        if (used == 0) break;
        pos += used;
    }
    pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(pos));
}

std::size_t LT7683Emu::parse(const std::uint8_t* p, std::size_t n) {
    const std::uint8_t op = p[0];
    // Before the first greeting nothing makes sense: look for it
    if (!hello_ && op != mp::kHello) { ++errors_; return 1; }
    switch (op) {
    case mp::kCmd:
        if (n < 2) return 0;
        cmd(p[1], true);
        return 2;
    case mp::kWrite1:
        if (n < 2) return 0;
        dataWrite(p[1]);
        return 2;
    case mp::kWrite:
    case mp::kRead: {
        if (n < 3) return 0;
        const std::size_t len = p[1] | (p[2] << 8);
        if (n < 3 + len) return 0;
        for (std::size_t i = 0; i < len; ++i) {
            if (op == mp::kWrite) dataWrite(p[3 + i]);
            else dataRead(p[3 + i]);
        }
        return 3 + len;
    }
    case mp::kReadRle: {
        if (n < 3) return 0;
        const std::size_t len = p[1] | (p[2] << 8);
        const std::size_t words = len / 2;
        // Complete yet?
        std::size_t i = 3, got = 0;
        while (got < words) {
            if (i >= n) return 0;
            const std::uint8_t t = p[i];
            if (t & 0x80) { if (i + 3 > n) return 0; got += (t & 0x7F) + 1u; i += 3; }
            else { const std::size_t c = t + 1u; if (i + 1 + 2 * c > n) return 0; got += c; i += 1 + 2 * c; }
        }
        if (len & 1) { if (i >= n) return 0; ++i; }
        // Decode
        std::size_t j = 3;
        got = 0;
        while (got < words) {
            const std::uint8_t t = p[j];
            if (t & 0x80) {
                for (int k = 0; k <= (t & 0x7F); ++k) { dataRead(p[j + 1]); dataRead(p[j + 2]); }
                got += (t & 0x7F) + 1u;
                j += 3;
            } else {
                for (int k = 0; k <= t; ++k) { dataRead(p[j + 1 + 2 * k]); dataRead(p[j + 2 + 2 * k]); }
                got += t + 1u;
                j += 1 + 2 * (t + 1u);
            }
        }
        if (len & 1) dataRead(p[j++]);
        return i;
    }
    case mp::kReset:
        reset();
        return 1;
    case mp::kMem: {
        if (n < 7) return 0;
        const std::uint32_t addr = static_cast<std::uint32_t>(p[1] | (p[2] << 8) | (p[3] << 16)) | (static_cast<std::uint32_t>(p[4]) << 24);
        const std::size_t len = p[5] | (p[6] << 8);
        if (n < 7 + len) return 0;
        memBytes(addr, p + 7, len);
        return 7 + len;
    }
    case mp::kMemRle: {
        if (n < 7) return 0;
        std::uint32_t addr = static_cast<std::uint32_t>(p[1] | (p[2] << 8) | (p[3] << 16)) | (static_cast<std::uint32_t>(p[4]) << 24);
        const std::size_t len = p[5] | (p[6] << 8);
        const std::size_t words = len / 2;
        std::size_t i = 7, got = 0;
        while (got < words) {                           // complete yet?
            if (i >= n) return 0;
            const std::uint8_t t = p[i];
            if (t & 0x80) { if (i + 3 > n) return 0; got += (t & 0x7F) + 1u; i += 3; }
            else { const std::size_t c = t + 1u; if (i + 1 + 2 * c > n) return 0; got += c; i += 1 + 2 * c; }
        }
        if (len & 1) { if (i >= n) return 0; ++i; }
        std::vector<std::uint8_t> out;
        out.reserve(len);
        std::size_t j = 7;
        got = 0;
        while (got < words) {
            const std::uint8_t t = p[j];
            if (t & 0x80) {
                for (int k = 0; k <= (t & 0x7F); ++k) { out.push_back(p[j + 1]); out.push_back(p[j + 2]); }
                got += (t & 0x7F) + 1u;
                j += 3;
            } else {
                out.insert(out.end(), p + j + 1, p + j + 1 + 2 * (t + 1u));
                got += t + 1u;
                j += 1 + 2 * (t + 1u);
            }
        }
        if (len & 1) out.push_back(p[j]);
        memBytes(addr, out.data(), out.size());
        return i;
    }
    case mp::kAssetUse:
    case mp::kAssetBegin: {
        if (n < 19) return 0;
        auto u32 = [&](int o) { return static_cast<std::uint32_t>(p[o] | (p[o + 1] << 8) | (p[o + 2] << 16)) | (static_cast<std::uint32_t>(p[o + 3]) << 24); };
        AssetArea a{ u32(1), u32(5), u32(9), u32(13), static_cast<std::uint16_t>(p[17] | (p[18] << 8)) };
        if (op == mp::kAssetBegin) { assetsBegun_.push_back(a); return 19; }
        std::vector<std::uint8_t> bytes;
        if (!loadAsset || !loadAsset(a.id, bytes) || bytes.size() != static_cast<std::size_t>(a.rowBytes) * a.rows) {
            ++errors_;                                  // HIPI thought we had it
            return 19;
        }
        for (std::uint32_t r = 0; r < a.rows; ++r)
            memBytes(a.addr + r * a.rowStep, bytes.data() + static_cast<std::size_t>(r) * a.rowBytes, a.rowBytes);
        return 19;
    }
    case mp::kAssetEnd: {
        if (n < 5) return 0;
        const std::uint32_t id = static_cast<std::uint32_t>(p[1] | (p[2] << 8) | (p[3] << 16)) | (static_cast<std::uint32_t>(p[4]) << 24);
        for (auto it = assetsBegun_.begin(); it != assetsBegun_.end(); ++it) {
            if (it->id != id) continue;
            std::vector<std::uint8_t> bytes;
            bytes.reserve(static_cast<std::size_t>(it->rowBytes) * it->rows);
            for (std::uint32_t r = 0; r < it->rows; ++r) {
                const std::uint32_t a = it->addr + r * it->rowStep;
                if (a + it->rowBytes > mem_.size()) break;
                bytes.insert(bytes.end(), mem_.begin() + a, mem_.begin() + a + it->rowBytes);
            }
            if (saveAsset && bytes.size() == static_cast<std::size_t>(it->rowBytes) * it->rows) saveAsset(id, bytes);
            assetsBegun_.erase(it);
            break;
        }
        return 5;
    }
    case mp::kHello: {
        if (n < 14) return 0;
        if (std::memcmp(p + 1, mp::kMagic, 7) != 0) { ++errors_; return 1; }
        reset();
        std::fill(mem_.begin(), mem_.end(), 0);
        width_ = p[10] | (p[11] << 8);
        height_ = p[12] | (p[13] << 8);
        if (width_ <= 0 || width_ > 2048) width_ = 1024;
        if (height_ <= 0 || height_ > 2048) height_ = 600;
        hello_ = true;
        mirrorMode_ = -1;                  // (the session says it again)
        bye_ = false;
        syncing_ = false;
        assetsBegun_.clear();
        return 14;
    }
    case mp::kSetReg:
        if (n < 3) return 0;
        setReg(p[1], p[2]);
        return 3;
    case mp::kSetPip:
        if (n < 4) return 0;
        if (p[1] < 2 && p[2] < 0x12) pip_[p[1]][p[2]] = p[3];
        return 4;
    case mp::kSetCur:
        if (n < 2) return 0;
        cmd(p[1], false);
        return 2;
    case mp::kFont:
        if (n < 1 + 4096) return 0;
        std::memcpy(font_, p + 1, 4096);
        fontKnown_ = true;
        return 1 + 4096;
    case mp::kSyncStart:
        if (n < 5) return 0;
        syncTotal_ = static_cast<std::uint32_t>(p[1] | (p[2] << 8) | (p[3] << 16) | (static_cast<std::uint32_t>(p[4]) << 24));
        syncDone_ = 0;
        syncing_ = true;
        return 5;
    case mp::kSyncDone:
        syncing_ = false;
        return 1;
    case mp::kMode:
        if (n < 2) return 0;
        mirrorMode_ = p[1];
        return 2;
    case mp::kBye:
        bye_ = true;
        return 1;
    default:
        ++errors_;
        return 1;
    }
}

// ── Register access ─────────────────────────────────────────────────────

void LT7683Emu::cmd(std::uint8_t reg, bool real) {
    cur_ = reg;
    if (real && reg == MRWDP) {
        dummy_ = true;          // the first read after selecting the port is a dummy
        wrLo_ = rdLo_ = -1;
        ucgHi_ = -1;
    }
}

void LT7683Emu::setReg(std::uint8_t r, std::uint8_t v) {
    regs_[r] = v;
}

void LT7683Emu::dataWrite(std::uint8_t v) {
    if (cur_ == MRWDP) memWrite(v);
    else writeReg(cur_, v);
}

void LT7683Emu::memBytes(std::uint32_t addr, const std::uint8_t* p, std::size_t n) {
    if (addr >= mem_.size()) return;
    n = std::min<std::size_t>(n, mem_.size() - addr);
    std::memcpy(mem_.data() + addr, p, n);
    if (syncing_) syncDone_ += static_cast<std::uint32_t>(n);
}

void LT7683Emu::dataRead(std::uint8_t v) {
    if (cur_ != MRWDP) return;
    memRead(true, v);
    if (syncing_) ++syncDone_;
}

std::uint8_t LT7683Emu::hostRead() {
    if (cur_ == MRWDP) return memRead(false, 0);
    return regs_[cur_];
}

void LT7683Emu::writeReg(std::uint8_t r, std::uint8_t v) {
    if (r >= mp::kPipRegFirst && r < mp::kPipRegFirst + mp::kPipRegCount)
        pip_[(regs_[mp::kRegMpwctr] >> 4) & 1][r - mp::kPipRegFirst] = v;
    regs_[r] = v;
    switch (r) {
    case 0x5F: case 0x60: case 0x61: case 0x62:          // graphic cursor moved
        wrLo_ = rdLo_ = -1;
        break;
    case 0x67:                                            // DCR0: line / triangle
        if (v & 0x80) { dcr0(v); regs_[0x67] = static_cast<std::uint8_t>(v & 0x7F); }
        break;
    case 0x76:                                            // DCR1: ellipse / rectangles
        if (v & 0x80) { dcr1(v); regs_[0x76] = static_cast<std::uint8_t>(v & 0x7F); }
        break;
    case 0x90:                                            // BTE start
        if (v & 0x10) bteStart();
        break;
    default:
        break;
    }
}

// ── Memory ──────────────────────────────────────────────────────────────

std::uint16_t LT7683Emu::pix(std::uint32_t addr) const {
    if (addr + 1 >= mem_.size()) return 0;
    return static_cast<std::uint16_t>(mem_[addr] | (mem_[addr + 1] << 8));
}

void LT7683Emu::setPix(std::uint32_t addr, std::uint16_t c) {
    if (addr + 1 >= mem_.size()) return;
    mem_[addr] = static_cast<std::uint8_t>(c);
    mem_[addr + 1] = static_cast<std::uint8_t>(c >> 8);
}

void LT7683Emu::memWrite(std::uint8_t v) {
    if (regs_[0x03] & 0x04) { textByte(v); return; }   // ICR: text mode
    if (bteMcu_) {
        if (wrLo_ < 0) { wrLo_ = v; return; }
        const std::uint16_t s0 = static_cast<std::uint16_t>(wrLo_ | (v << 8));
        wrLo_ = -1;
        bteMcuPixel(s0);
        return;
    }
    if (regs_[0x5E] & 0x04) {                            // linear addressing
        std::uint32_t a = r32(0x5F);
        if (a < mem_.size()) mem_[a] = v;
        ++a;
        w16(0x5F, a & 0xFFFF);
        w16(0x61, a >> 16);
        return;
    }
    // 16bpp block mode: two bytes per pixel, low first, at the graphic
    // cursor; the cursor moves right and wraps inside the Active Window
    if (wrLo_ < 0) { wrLo_ = v; return; }
    const std::uint16_t c = static_cast<std::uint16_t>(wrLo_ | (v << 8));
    wrLo_ = -1;
    int x = r16(0x5F), y = r16(0x61);
    plot(x, y, c);
    const int ax = r16(0x56), ay = r16(0x58), aw = std::max<int>(1, r16(0x5A)), ah = std::max<int>(1, r16(0x5C));
    if (++x >= ax + aw) { x = ax; if (++y >= ay + ah) y = ay; }
    w16(0x5F, static_cast<std::uint32_t>(x));
    w16(0x61, static_cast<std::uint32_t>(y));
}

// A memory read: apply = take v as the content (mirror stream), else
// return the content (raw SPI emulation). Same addressing as writes.
std::uint8_t LT7683Emu::memRead(bool apply, std::uint8_t v) {
    if (dummy_) { dummy_ = false; return 0; }
    if (regs_[0x5E] & 0x04) {                            // linear
        std::uint32_t a = r32(0x5F);
        std::uint8_t out = 0;
        if (a < mem_.size()) { if (apply) mem_[a] = v; out = mem_[a]; }
        ++a;
        w16(0x5F, a & 0xFFFF);
        w16(0x61, a >> 16);
        return out;
    }
    int x = r16(0x5F), y = r16(0x61);
    const std::uint32_t addr = r32(0x50) + 2u * (static_cast<std::uint32_t>(y) * r16(0x54) + static_cast<std::uint32_t>(x));
    std::uint8_t out = 0;
    const std::uint32_t a = addr + (rdLo_ < 0 ? 0 : 1);
    if (a < mem_.size()) { if (apply) mem_[a] = v; out = mem_[a]; }
    if (rdLo_ < 0) { rdLo_ = 1; return out; }
    rdLo_ = -1;
    const int ax = r16(0x56), ay = r16(0x58), aw = std::max<int>(1, r16(0x5A)), ah = std::max<int>(1, r16(0x5C));
    if (++x >= ax + aw) { x = ax; if (++y >= ay + ah) y = ay; }
    w16(0x5F, static_cast<std::uint32_t>(x));
    w16(0x61, static_cast<std::uint32_t>(y));
    return out;
}

// ── Canvas drawing ──────────────────────────────────────────────────────

std::uint16_t LT7683Emu::fg() const {
    return static_cast<std::uint16_t>(((regs_[0xD2] >> 3) << 11) | ((regs_[0xD3] >> 2) << 5) | (regs_[0xD4] >> 3));
}
std::uint16_t LT7683Emu::bg() const {
    return static_cast<std::uint16_t>(((regs_[0xD5] >> 3) << 11) | ((regs_[0xD6] >> 2) << 5) | (regs_[0xD7] >> 3));
}

void LT7683Emu::plot(int x, int y, std::uint16_t c) {
    const int ax = r16(0x56), ay = r16(0x58), aw = r16(0x5A), ah = r16(0x5C);
    if (x < ax || y < ay || x >= ax + aw || y >= ay + ah) return;
    const int stride = r16(0x54);
    if (x >= stride) return;
    setPix(r32(0x50) + 2u * (static_cast<std::uint32_t>(y) * static_cast<std::uint32_t>(stride) + static_cast<std::uint32_t>(x)), c);
}

void LT7683Emu::span(int x0, int x1, int y, std::uint16_t c) {
    if (x0 > x1) std::swap(x0, x1);
    for (int x = x0; x <= x1; ++x) plot(x, y, c);
}

void LT7683Emu::line(int x0, int y0, int x1, int y1, std::uint16_t c) {
    const int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    const int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        plot(x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        const int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

namespace {
// Half-width of an ellipse with radii a, b at vertical distance dy
int ellipseDx(int a, int b, int dy) {
    if (b <= 0) return a;
    const double t = 1.0 - (static_cast<double>(dy) * dy) / (static_cast<double>(b) * b);
    return t <= 0 ? 0 : static_cast<int>(std::lround(a * std::sqrt(t)));
}
}

void LT7683Emu::ellipse(int cx, int cy, int a, int b, bool fill, std::uint16_t c) {
    if (fill) {
        for (int dy = -b; dy <= b; ++dy) {
            const int dx = ellipseDx(a, b, dy);
            span(cx - dx, cx + dx, cy + dy, c);
        }
        return;
    }
    // Outline: points from both directions, so steep and flat parts close
    for (int dy = 0; dy <= b; ++dy) {
        const int dx = ellipseDx(a, b, dy);
        plot(cx + dx, cy + dy, c); plot(cx - dx, cy + dy, c);
        plot(cx + dx, cy - dy, c); plot(cx - dx, cy - dy, c);
    }
    for (int dx = 0; dx <= a; ++dx) {
        const int dy = ellipseDx(b, a, dx);
        plot(cx + dx, cy + dy, c); plot(cx - dx, cy + dy, c);
        plot(cx + dx, cy - dy, c); plot(cx - dx, cy - dy, c);
    }
}

void LT7683Emu::roundRect(int x0, int y0, int x1, int y1, int a, int b, bool fill, std::uint16_t c) {
    if (x0 > x1) std::swap(x0, x1);
    if (y0 > y1) std::swap(y0, y1);
    a = std::min(a, (x1 - x0) / 2);
    b = std::min(b, (y1 - y0) / 2);
    if (fill) {
        for (int y = y0; y <= y1; ++y) {
            int dy = 0;
            if (y < y0 + b) dy = y0 + b - y;
            else if (y > y1 - b) dy = y - (y1 - b);
            const int inset = a - ellipseDx(a, b, dy);
            span(x0 + inset, x1 - inset, y, c);
        }
        return;
    }
    span(x0 + a, x1 - a, y0, c);
    span(x0 + a, x1 - a, y1, c);
    for (int y = y0 + b; y <= y1 - b; ++y) { plot(x0, y, c); plot(x1, y, c); }
    const int lx = x0 + a, rx = x1 - a, ty = y0 + b, by = y1 - b;
    for (int dy = 0; dy <= b; ++dy) {
        const int dx = ellipseDx(a, b, dy);
        plot(rx + dx, by + dy, c); plot(lx - dx, by + dy, c);
        plot(rx + dx, ty - dy, c); plot(lx - dx, ty - dy, c);
    }
    for (int dx = 0; dx <= a; ++dx) {
        const int dy = ellipseDx(b, a, dx);
        plot(rx + dx, by + dy, c); plot(lx - dx, by + dy, c);
        plot(rx + dx, ty - dy, c); plot(lx - dx, ty - dy, c);
    }
}

void LT7683Emu::triangle(int x0, int y0, int x1, int y1, int x2, int y2, bool fill, std::uint16_t c) {
    if (!fill) {
        line(x0, y0, x1, y1, c);
        line(x1, y1, x2, y2, c);
        line(x2, y2, x0, y0, c);
        return;
    }
    const int ymin = std::min({ y0, y1, y2 }), ymax = std::max({ y0, y1, y2 });
    const int xs[3] = { x0, x1, x2 }, ys[3] = { y0, y1, y2 };
    for (int y = ymin; y <= ymax; ++y) {
        double lo = 1e9, hi = -1e9;
        for (int e = 0; e < 3; ++e) {
            const int ax = xs[e], ay = ys[e], bx = xs[(e + 1) % 3], by = ys[(e + 1) % 3];
            if (ay == by) {
                if (y == ay) { lo = std::min<double>({ lo, double(ax), double(bx) }); hi = std::max<double>({ hi, double(ax), double(bx) }); }
                continue;
            }
            if (y < std::min(ay, by) || y > std::max(ay, by)) continue;
            const double x = ax + (static_cast<double>(y) - ay) * (bx - ax) / (by - ay);
            lo = std::min(lo, x);
            hi = std::max(hi, x);
        }
        if (lo <= hi) span(static_cast<int>(std::lround(lo)), static_cast<int>(std::lround(hi)), y, c);
    }
}

void LT7683Emu::dcr0(std::uint8_t v) {
    const int x1 = s16(r16(0x68)), y1 = s16(r16(0x6A)), x2 = s16(r16(0x6C)), y2 = s16(r16(0x6E));
    const int x3 = s16(r16(0x70)), y3 = s16(r16(0x72));
    const bool fill = (v & 0x20) != 0;
    switch ((v >> 1) & 0x0F) {
    case 0: line(x1, y1, x2, y2, fg()); break;
    case 1: triangle(x1, y1, x2, y2, x3, y3, fill, fg()); break;
    default: break;
    }
}

void LT7683Emu::dcr1(std::uint8_t v) {
    const bool fill = (v & 0x40) != 0;
    const int a = r16(0x77), b = r16(0x79);
    const int x1 = s16(r16(0x68)), y1 = s16(r16(0x6A)), x2 = s16(r16(0x6C)), y2 = s16(r16(0x6E));
    switch ((v >> 4) & 0x03) {
    case 0: ellipse(s16(r16(0x7B)), s16(r16(0x7D)), a, b, fill, fg()); break;
    case 2: roundRect(x1, y1, x2, y2, 0, 0, fill, fg()); break;
    case 3: roundRect(x1, y1, x2, y2, a, b, fill, fg()); break;
    default: break;      // quarter curves: not used by HIPI
    }
}

// ── Text engine ─────────────────────────────────────────────────────────

void LT7683Emu::textByte(std::uint8_t v) {
    if ((regs_[0xCC] & 0xC0) == 0x80) {                  // user-defined: 2-byte codes, high first
        if (ucgHi_ < 0) { ucgHi_ = v; return; }
        const std::uint32_t code = static_cast<std::uint32_t>(ucgHi_ << 8) | v;
        ucgHi_ = -1;
        drawChar(code);
        return;
    }
    drawChar(v);
}

void LT7683Emu::drawChar(std::uint32_t code) {
    std::uint8_t g[16];
    if ((regs_[0xCC] & 0xC0) == 0x80) {
        const std::uint32_t a = r32(0xDB) + code * 16u;
        for (int i = 0; i < 16; ++i) g[i] = a + i < mem_.size() ? mem_[a + i] : 0;
    } else {
        std::memcpy(g, font_[code & 0xFF], 16);
    }
    const std::uint8_t ccr1 = regs_[0xCD];
    const int hs = ((ccr1 >> 2) & 3) + 1, vs = (ccr1 & 3) + 1;
    const bool transparent = (ccr1 & 0x40) != 0;
    const int cw = 8 * hs, ch = 16 * vs;
    const int ax = r16(0x56), aw = r16(0x5A);
    int x = r16(0x63), y = r16(0x65);
    if (x + cw > ax + aw && x > ax) {                    // no room left: next line
        x = ax;
        y += ch + (regs_[0xD0] & 0x1F);
    }
    const std::uint16_t f = fg(), b = bg();
    for (int row = 0; row < 16; ++row)
        for (int col = 0; col < 8; ++col) {
            const bool on = (g[row] & (0x80 >> col)) != 0;
            if (!on && transparent) continue;
            const std::uint16_t c = on ? f : b;
            for (int sy = 0; sy < vs; ++sy)
                for (int sx = 0; sx < hs; ++sx) plot(x + col * hs + sx, y + row * vs + sy, c);
        }
    x += cw + (regs_[0xD1] & 0x3F);
    w16(0x63, static_cast<std::uint32_t>(x));
    w16(0x65, static_cast<std::uint32_t>(y));
}

// ── BTE ─────────────────────────────────────────────────────────────────

void LT7683Emu::bteStart() {
    const int op = regs_[0x91] & 0x0F, code = regs_[0x91] >> 4;
    const std::uint32_t s0a = r32(0x93), s1a = r32(0x9D), dta = r32(0xA7);
    const int s0w = r16(0x97), s0x = r16(0x99), s0y = r16(0x9B);
    const int s1w = r16(0xA1), s1x = r16(0xA3), s1y = r16(0xA5);
    const int dtw = r16(0xAB), dtx = r16(0xAD), dty = r16(0xAF);
    const int w = r16(0xB1), h = r16(0xB3);
    auto at = [](std::uint32_t base, int stride, int x, int y) {
        return base + 2u * (static_cast<std::uint32_t>(y) * static_cast<std::uint32_t>(stride) + static_cast<std::uint32_t>(x));
    };
    switch (op) {
    case 0x0:                                            // MCU write with ROP: data follows
        bteMcu_ = w > 0 && h > 0;
        bteCount_ = 0;
        wrLo_ = -1;
        if (bteMcu_) return;
        break;
    case 0x2: {                                          // memory copy with ROP
        std::vector<std::uint16_t> tmp(static_cast<std::size_t>(w) * h);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                tmp[static_cast<std::size_t>(y) * w + x] =
                    rop(code, pix(at(s0a, s0w, s0x + x, s0y + y)), pix(at(s1a, s1w, s1x + x, s1y + y)));
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) setPix(at(dta, dtw, dtx + x, dty + y), tmp[static_cast<std::size_t>(y) * w + x]);
        break;
    }
    case 0xC:                                            // solid fill
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) setPix(at(dta, dtw, dtx + x, dty + y), fg());
        break;
    default:                                             // not used by HIPI
        break;
    }
    regs_[0x90] = static_cast<std::uint8_t>(regs_[0x90] & ~0x10);
}

void LT7683Emu::bteMcuPixel(std::uint16_t s0) {
    const int w = std::max<int>(1, r16(0xB1)), h = r16(0xB3);
    const int x = static_cast<int>(bteCount_ % static_cast<std::uint32_t>(w));
    const int y = static_cast<int>(bteCount_ / static_cast<std::uint32_t>(w));
    const std::uint32_t s1 = r32(0x9D) + 2u * (static_cast<std::uint32_t>(r16(0xA5) + y) * r16(0xA1) + r16(0xA3) + x);
    const std::uint32_t d = r32(0xA7) + 2u * (static_cast<std::uint32_t>(r16(0xAF) + y) * r16(0xAB) + r16(0xAD) + x);
    setPix(d, rop(regs_[0x91] >> 4, s0, pix(s1)));
    if (++bteCount_ >= static_cast<std::uint32_t>(w) * static_cast<std::uint32_t>(h)) {
        bteMcu_ = false;
        regs_[0x90] = static_cast<std::uint8_t>(regs_[0x90] & ~0x10);
    }
}

// ── Output ──────────────────────────────────────────────────────────────

void LT7683Emu::render(std::uint32_t* out, double timeSec, bool backlight) const {
    const int W = width_, H = height_;
    const std::size_t total = static_cast<std::size_t>(W) * static_cast<std::size_t>(H);
    if (!(regs_[0x12] & 0x40)) {                          // display off
        std::fill(out, out + total, 0xFF000000u);
        return;
    }
    std::vector<std::uint16_t> px(total);
    const std::uint32_t misa = r32(0x20);
    int miw = r16(0x24);
    if (miw == 0) miw = W;
    const int mwx = r16(0x26), mwy = r16(0x28);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            px[static_cast<std::size_t>(y) * W + x] =
                pix(misa + 2u * (static_cast<std::uint32_t>(mwy + y) * static_cast<std::uint32_t>(miw) + static_cast<std::uint32_t>(mwx + x)));

    // Text cursor (on the main window, under the PIP windows)
    if ((regs_[0x3C] & 0x02) && r32(0x50) == misa) {
        bool on = true;
        if (regs_[0x3C] & 0x01) on = static_cast<long>(timeSec * 60.0 / (regs_[0x3D] + 1)) % 2 == 0;
        if (on) {
            const int hs = ((regs_[0xCD] >> 2) & 3) + 1, vs = (regs_[0xCD] & 3) + 1;
            const int cw = (regs_[0x3E] + 1) * hs, chh = (regs_[0x3F] + 1) * vs;
            const int cx = r16(0x63) - mwx, cy = r16(0x65) - mwy + 16 * vs - chh;
            const std::uint16_t c = fg();
            for (int y = std::max(0, cy); y < std::min(H, cy + chh); ++y)
                for (int x = std::max(0, cx); x < std::min(W, cx + cw); ++x) px[static_cast<std::size_t>(y) * W + x] = c;
        }
    }

    // PIP-2, then PIP-1 on top
    for (int set : { 1, 0 }) {
        if (!(regs_[0x10] & (set == 0 ? 0x80 : 0x40))) continue;
        const int dx = p16(set, 0), dy = p16(set, 2);
        const std::uint32_t isa = static_cast<std::uint32_t>(p16(set, 4)) | (static_cast<std::uint32_t>(p16(set, 6)) << 16);
        const int iw = p16(set, 8), ix = p16(set, 10), iy = p16(set, 12), w = p16(set, 14), h = p16(set, 16);
        for (int y = 0; y < h; ++y) {
            const int sy = dy + y;
            if (sy < 0 || sy >= H) continue;
            for (int x = 0; x < w; ++x) {
                const int sx = dx + x;
                if (sx < 0 || sx >= W) continue;
                px[static_cast<std::size_t>(sy) * W + sx] =
                    pix(isa + 2u * (static_cast<std::uint32_t>(iy + y) * static_cast<std::uint32_t>(iw) + static_cast<std::uint32_t>(ix + x)));
            }
        }
    }

    // Backlight: PWM0 duty / period (when the PWM runs)
    int level = 256;
    if (backlight && (regs_[0x85] & 0x03) == 0x02 && (regs_[0x86] & 0x01)) {
        const int period = r16(0x8A), duty = r16(0x88);
        if (period > 0) level = std::min(256, duty * 256 / period);
    }
    for (std::size_t i = 0; i < total; ++i) {
        const std::uint16_t c = px[i];
        std::uint32_t r = ((c >> 11) & 0x1F), g = ((c >> 5) & 0x3F), b = (c & 0x1F);
        r = (r << 3) | (r >> 2);
        g = (g << 2) | (g >> 4);
        b = (b << 3) | (b >> 2);
        if (level < 256) { r = r * level >> 8; g = g * level >> 8; b = b * level >> 8; }
        out[i] = 0xFF000000u | (r << 16) | (g << 8) | b;
    }
}
