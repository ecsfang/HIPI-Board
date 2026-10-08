// mirror_encoder.hpp -- turns the LT7683's SPI transactions into the
// display mirror stream (mirror_protocol.h). Header-only and free of Pico
// SDK dependencies, so the same code runs in the firmware
// (display_mirror.cpp) and in tools/hipiview's host test.
//
// Fed by a transport wrapper: onCsLow() / onTransfer() / onCsHigh() for
// every SPI transaction the display driver makes. It always keeps a
// shadow copy of every register written (cheap), so a viewer that
// connects later can be given the current register state; packets are
// only produced while enabled.
//
// Packets are collected in a small buffer and handed to the sink at
// transaction boundaries (CS high) once it fills up, and on flush() --
// called from the main loop. Consecutive data writes (or MRWDP reads)
// with no register change in between are merged into one packet.
#pragma once
#include "mirror_protocol.h"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

namespace hipi {
namespace mirror {

class Sink {
public:
    virtual ~Sink() = default;
    // Takes n bytes. mayBlock: the caller is between SPI transactions and
    // the sink may wait for room. Returns false if the data couldn't be
    // taken (the session is then out of step and needs a new sync).
    virtual bool put(const std::uint8_t* data, std::size_t n, bool mayBlock) = 0;
};

class Encoder {
public:
    explicit Encoder(Sink& sink) : sink_(sink) { std::memset(regs_, 0, sizeof(regs_)); std::memset(pip_, 0, sizeof(pip_)); }
    ~Encoder() { release(); }

    // ── Session control ─────────────────────────────────────────────────
    // The packet buffers (8 kB) are only allocated while enabled, so the
    // mirror costs no RAM when no viewer is connected. Returns false if
    // they couldn't be allocated (then it stays disabled).
    bool setEnabled(bool on) {
        if (on && !enabled_) {
            if (buf_ == nullptr) buf_ = new (std::nothrow) std::uint8_t[kBufSize];
            if (rle_ == nullptr) rle_ = new (std::nothrow) std::uint8_t[kRleSize];
            if (buf_ == nullptr || rle_ == nullptr) { release(); return false; }
            size_ = 0; openType_ = 0; lastCmd_ = -1; overflow_ = false;
        }
        enabled_ = on;
        suspended_ = false;
        if (!on) { size_ = 0; openType_ = 0; release(); }
        return true;
    }
    bool enabled() const { return enabled_; }
    // The sink refused data: the viewer has lost track; resync needed
    bool overflowed() const { return overflow_; }

    // Hands everything collected so far to the sink (main loop)
    void flush() { closeOpen(); push(true); }

    // A session packet from the firmware itself (hello, snapshot, font...)
    void emit(const std::uint8_t* data, std::size_t n) {
        if (!enabled_) return;
        closeOpen();
        lastCmd_ = -1;
        if (size_ + n > kBufSize) push(true);
        if (n > kBufSize) { if (!sink_.put(data, n, true)) fail(); return; }
        std::memcpy(buf_ + size_, data, n);
        size_ += n;
    }

    // ── Read-back without per-byte packets ──────────────────────────────
    // Between suspend() and resume() no SPI traffic is sent (the shadow
    // registers are still kept). resume() then sends every register that
    // changed meanwhile as kSetReg/kSetPip, so the viewer's registers are
    // in step again. Used around an SDRAM read whose data is then sent
    // as one kMem packet (emitMem) -- much less work per byte than sending
    // each read as MRWDP read data.
    void suspend() {
        if (!enabled_ || suspended_) return;
        closeOpen();
        std::memcpy(savedRegs_, regs_, sizeof(regs_));
        std::memcpy(savedPip_, pip_, sizeof(pip_));
        suspended_ = true;
    }
    void resume() {
        if (!suspended_) return;
        suspended_ = false;
        if (!enabled_) return;
        for (int r = 0; r < 256; ++r) {
            if (r == kRegMrwdp || (r >= kPipRegFirst && r < kPipRegFirst + kPipRegCount)) continue;
            if (regs_[r] == savedRegs_[r]) continue;
            const std::uint8_t p[3] = { kSetReg, static_cast<std::uint8_t>(r), regs_[r] };
            emit(p, 3);
        }
        for (int set = 0; set < 2; ++set)
            for (int i = 0; i < kPipRegCount; ++i) {
                if (pip_[set][i] == savedPip_[set][i]) continue;
                const std::uint8_t p[4] = { kSetPip, static_cast<std::uint8_t>(set), static_cast<std::uint8_t>(i), pip_[set][i] };
                emit(p, 4);
            }
        const std::uint8_t c[2] = { kSetCur, curReg_ };
        emit(c, 2);
    }
    // n bytes of SDRAM content at addr (n <= 4096), run-length coded if
    // that's smaller
    void emitMem(std::uint32_t addr, const std::uint8_t* data, std::size_t n) {
        if (!enabled_ || n == 0 || n > kBufSize - 16) return;
        const std::size_t z = rleEncode(data, n, rle_);
        const bool packed = z + 8 < n;
        std::uint8_t hdr[7] = { packed ? kMemRle : kMem,
            static_cast<std::uint8_t>(addr), static_cast<std::uint8_t>(addr >> 8),
            static_cast<std::uint8_t>(addr >> 16), static_cast<std::uint8_t>(addr >> 24),
            static_cast<std::uint8_t>(n), static_cast<std::uint8_t>(n >> 8) };
        closeOpen();
        lastCmd_ = -1;
        const std::size_t body = packed ? z : n;
        if (size_ + sizeof(hdr) + body > kBufSize) push(true);
        if (!enabled_) return;
        std::memcpy(buf_ + size_, hdr, sizeof(hdr));
        size_ += sizeof(hdr);
        std::memcpy(buf_ + size_, packed ? rle_ : data, body);
        size_ += body;
    }

    // ── Shadow registers ────────────────────────────────────────────────
    std::uint8_t reg(std::uint8_t r) const { return regs_[r]; }
    std::uint8_t pip(int set, int idx) const { return pip_[set][idx]; }
    std::uint8_t curReg() const { return curReg_; }

    // ── Fed by the transport ────────────────────────────────────────────
    void onReset() {
        std::memset(regs_, 0, sizeof(regs_));
        std::memset(pip_, 0, sizeof(pip_));
        if (!active()) return;
        closeOpen();
        put1(kReset);
        lastCmd_ = -1;
    }
    void onCsLow() { first_ = true; }
    void onCsHigh() {
        first_ = true;
        if (active() && size_ >= kPushAt) { closeOpen(); push(true); }
    }
    // Called after the real transfer, so rx (if any) holds what was read
    void onTransfer(const std::uint8_t* tx, const std::uint8_t* rx, std::size_t len) {
        std::size_t i = 0;
        if (first_) {
            if (tx == nullptr || len == 0) return;
            type_ = static_cast<std::uint8_t>(tx[0] & 0xC0);
            first_ = false;
            i = 1;
        }
        switch (type_) {
        case kLtCmdWrite:
            for (; i < len; ++i) cmd(tx[i]);
            break;
        case kLtDataWrite:
            // Pixel data with no viewer: nothing to keep (fast path for bitmaps)
            if (!active() && curReg_ == kRegMrwdp) break;
            if (tx != nullptr) for (; i < len; ++i) dataWrite(tx[i]);
            break;
        case kLtDataRead:
            if (active() && rx != nullptr && curReg_ == kRegMrwdp) for (; i < len; ++i) dataRead(rx[i]);
            break;
        default:   // status read: changes nothing
            break;
        }
    }

private:
    static constexpr std::size_t kBufSize = 4096;
    static constexpr std::size_t kRleSize = kBufSize + kBufSize / 128 + 8;
    static constexpr std::size_t kPushAt = 1024;

    void release() {
        delete[] buf_;
        delete[] rle_;
        buf_ = nullptr;
        rle_ = nullptr;
    }
    static constexpr std::uint8_t kOpenWrite = 1, kOpenRead = 2;

    bool active() const { return enabled_ && !suspended_; }

    void cmd(std::uint8_t r) {
        curReg_ = r;
        if (!active()) return;
        // Re-selecting the same register changes nothing -- except MRWDP,
        // where the selection itself makes the next read a dummy read
        if (r == lastCmd_ && r != kRegMrwdp) { closeOpen(); return; }
        closeOpen();
        put1(kCmd);
        put1(r);
        lastCmd_ = r;
    }
    void dataWrite(std::uint8_t v) {
        const std::uint8_t r = curReg_;
        if (r != kRegMrwdp) {
            regs_[r] = v;
            if (r == kRegMpwctr) pipSel_ = (v & 0x10) ? 1 : 0;
            if (r >= kPipRegFirst && r < kPipRegFirst + kPipRegCount) pip_[pipSel_][r - kPipRegFirst] = v;
        }
        if (active()) append(kOpenWrite, v);
    }
    void dataRead(std::uint8_t v) {
        if (active()) append(kOpenRead, v);
    }

    // ── Packet buffer ───────────────────────────────────────────────────
    void put1(std::uint8_t b) {
        if (size_ + 1 > kBufSize) push(false);
        buf_[size_++] = b;
    }
    void append(std::uint8_t type, std::uint8_t v) {
        if (openType_ != type || openLen_ == 0xFFFF || size_ + 1 > kBufSize) {
            closeOpen();
            if (size_ + 4 > kBufSize) push(false);
            openType_ = type;
            openHdr_ = size_;
            openLen_ = 0;
            buf_[size_++] = type == kOpenWrite ? kWrite : kRead;
            buf_[size_++] = 0;
            buf_[size_++] = 0;
        }
        buf_[size_++] = v;
        ++openLen_;
    }
    void closeOpen() {
        if (openType_ == 0) return;
        const std::size_t data = openHdr_ + 3;
        if (openType_ == kOpenWrite && openLen_ == 1) {
            buf_[openHdr_] = kWrite1;
            buf_[openHdr_ + 1] = buf_[data];
            size_ = openHdr_ + 2;
        } else {
            buf_[openHdr_ + 1] = static_cast<std::uint8_t>(openLen_ & 0xFF);
            buf_[openHdr_ + 2] = static_cast<std::uint8_t>(openLen_ >> 8);
            if (openType_ == kOpenRead && openLen_ >= 16) {
                const std::size_t n = rleEncode(buf_ + data, openLen_, rle_);
                if (n + 8 < openLen_) {
                    buf_[openHdr_] = kReadRle;
                    std::memcpy(buf_ + data, rle_, n);
                    size_ = data + n;
                }
            }
        }
        openType_ = 0;
    }
    // Inside an SPI transaction (CS low) the sink must not wait: tud_task()
    // there could run other SPI traffic. Then a full sink means overflow.
    void push(bool mayBlock) {
        if (size_ == 0) return;
        if (openType_ != 0) {
            // Keep the open packet's start in the buffer: hand over only
            // what precedes it
            const std::size_t head = openHdr_;
            if (head > 0) {
                if (!sink_.put(buf_, head, mayBlock)) { fail(); return; }
                std::memmove(buf_, buf_ + head, size_ - head);
                size_ -= head;
                openHdr_ = 0;
            }
            if (size_ + 4 <= kBufSize) return;
            closeOpen();            // the open packet itself fills the buffer
        }
        if (!sink_.put(buf_, size_, mayBlock)) fail();
        size_ = 0;
    }
    void fail() {
        overflow_ = true;
        enabled_ = false;
        size_ = 0;
        openType_ = 0;
    }

    Sink& sink_;
    bool enabled_ = false;
    bool suspended_ = false;
    bool overflow_ = false;
    std::uint8_t savedRegs_[256];
    std::uint8_t savedPip_[2][kPipRegCount];
    bool first_ = true;
    std::uint8_t type_ = 0;
    std::uint8_t curReg_ = 0;
    int lastCmd_ = -1;
    int pipSel_ = 0;
    std::uint8_t regs_[256];
    std::uint8_t pip_[2][kPipRegCount];

    std::uint8_t* buf_ = nullptr;
    std::uint8_t* rle_ = nullptr;
    std::size_t size_ = 0;
    std::uint8_t openType_ = 0;
    std::size_t openHdr_ = 0;
    std::size_t openLen_ = 0;
};

}  // namespace mirror
}  // namespace hipi
