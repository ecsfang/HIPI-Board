// mirror_protocol.h -- the display mirror stream (USB CDC3, "HIPI Display
// Mirror"): everything HIPI sends to the LT7683 display controller over
// SPI, so a PC program (tools/hipiview) can run an emulated LT7683 and
// show -- and record -- exactly what is on the panel.
//
// Shared by the firmware (display_mirror.cpp) and tools/hipiview, so it
// must stay plain C++17 with no Pico SDK dependencies.
//
// The viewer starts a session by opening the port and sending
//   'H''I''P''I''V''I''E''W' n id1 id2 ... idn      (ids: 4 bytes each, LE)
// (again whenever it wants a fresh start). The ids are the layer pictures
// ("assets", see kAssetUse) the viewer has in its cache from earlier
// sessions; n may be 0. Until a request arrives nothing is sent, so
// programs that merely probe serial ports set nothing off.
//
// FRAMING: on the wire, the stream travels in numbered frames
//   0xA5 0x5A sid len(2) pos(4) crc(2) payload(len)
//   sid  session id (changes with every new session)
//   pos  where this frame starts in the session's frame stream (bytes,
//        frame headers included; the first frame is at 0)
//   crc  CRC-16/CCITT over sid, len, pos and the payload
// USB can lose a packet now and then, so the link is made reliable: the
// viewer acknowledges what it got -- 'K' sid pos(4): everything before pos
// arrived -- and asks for a resend when a frame is missing -- 'N' sid
// pos(4): send again from pos. HIPI keeps every frame until it is
// acknowledged. The packets below are the frames' payloads, one after
// another (a packet may span frames).
// With nothing to send, HIPI sends an empty frame (len 0) every second. A
// viewer that hears nothing for a few seconds knows the session has ended
// and asks for a new one; a viewer that has everything acknowledges a
// resent frame again (its 'K' may have been lost). HIPI ends a session
// whose viewer has stopped reading -- nothing acknowledged, not a word,
// for 3 s -- and then waits for a new request.
//
// The stream is a sequence of packets, each starting with one opcode byte.
// Multi-byte numbers are little-endian.
//
// SPI traffic (mirrors the controller's own 4-wire SPI cycles):
//   kCmd     reg                      register address write (selects reg)
//   kWrite1  byte                     one data write to the selected reg
//   kWrite   lenLo lenHi bytes...     len data writes to the selected reg
//   kRead    lenLo lenHi bytes...     len data reads from MRWDP (memory
//                                     data port) -- the values the chip
//                                     returned. Reads of other registers
//                                     and status reads are not sent: they
//                                     don't change anything.
//   kReadRle lenLo lenHi rle...       same as kRead, run-length coded (see
//                                     below); len = decoded byte count
//   kReset                            hardware reset of the controller
//
// SDRAM contents (the read-back when a session starts):
//   kMem     addr(4) len(2) bytes...  len bytes of SDRAM from addr
//   kMemRle  addr(4) len(2) rle...    the same, run-length coded (below)
//
// Session control (sent by the firmware itself, not SPI traffic):
//   kHello   'H''I''P''I''M''I''R' version chip widthLo widthHi heightLo heightHi
//                                     start of a session: the viewer resets
//                                     its emulator. chip: kChipLT7683.
//   kSetReg  reg value                set a register WITHOUT any side effect
//                                     (no drawing, no BTE start) -- the
//                                     register snapshot at the start of a
//                                     session
//   kSetPip  pip index value          same for the per-PIP window registers
//                                     REG[2Ah]..REG[3Bh] (pip 0 = PIP-1,
//                                     1 = PIP-2; index = reg - 0x2A)
//   kSetCur  reg                      the currently selected register,
//                                     without the dummy-read side effect
//                                     of a real kCmd for MRWDP
//   kFont    4096 bytes               the controller's built-in 8x16 font
//                                     (CGROM, ISO 8859-1): 256 chars x 16
//                                     rows, MSB = leftmost pixel. Captured
//                                     from the real chip, so the viewer
//                                     doesn't need its own copy.
//   kSyncStart total(4)               the SDRAM read-back starts; total =
//                                     the number of bytes it will read (for
//                                     a progress display)
//   kSyncDone                         read-back done: the emulated SDRAM
//                                     now matches the real one
//
// Assets -- layers filled once from a fixed source (the Tape view pictures
// from the SD card), identified by an id made from that source (file name,
// size, date, position). The viewer keeps them in a cache, so they only
// travel the first time:
//   kAssetUse   id(4) addr(4) rowBytes(4) rowStep(4) rows(2)
//               the viewer has this asset (it said so in its request):
//               put it into SDRAM, rows x rowBytes from addr, rowStep apart
//   kAssetBegin (same fields)         the area that follows (kMem) is this
//                                     asset: keep it in the cache ...
//   kAssetEnd   id(4)                 ... once this arrives
//
// RLE (kReadRle, kMemRle): 16-bit little-endian words. A token byte t:
//   t & 0x80 == 0: (t + 1) literal words follow (2 bytes each)
//   t & 0x80 != 0: the next word repeats ((t & 0x7F) + 1) times
// A trailing odd byte (if len is odd) follows as-is after the tokens.
#pragma once
#include <cstddef>
#include <cstdint>

namespace hipi {
namespace mirror {

constexpr std::uint8_t kCmd       = 0x01;
constexpr std::uint8_t kWrite1    = 0x02;
constexpr std::uint8_t kWrite     = 0x03;
constexpr std::uint8_t kRead      = 0x04;
constexpr std::uint8_t kReadRle   = 0x05;
constexpr std::uint8_t kReset     = 0x06;
constexpr std::uint8_t kMem       = 0x07;
constexpr std::uint8_t kMemRle    = 0x08;

constexpr std::uint8_t kHello     = 0x10;
constexpr std::uint8_t kSetReg    = 0x11;
constexpr std::uint8_t kSetPip    = 0x12;
constexpr std::uint8_t kSetCur    = 0x13;
constexpr std::uint8_t kFont      = 0x14;
constexpr std::uint8_t kSyncStart = 0x15;
constexpr std::uint8_t kSyncDone  = 0x16;
constexpr std::uint8_t kAssetUse  = 0x17;
constexpr std::uint8_t kAssetBegin = 0x18;
constexpr std::uint8_t kAssetEnd  = 0x19;
constexpr int kMaxViewerAssets = 32;     // ids a request may list

constexpr std::uint8_t kVersion   = 4;
constexpr std::uint8_t kFrame0    = 0xA5;
constexpr std::uint8_t kFrame1    = 0x5A;
constexpr std::size_t  kFrameHeader = 11;
constexpr std::uint8_t kChipLT7683 = 1;
constexpr char kMagic[7] = { 'H', 'I', 'P', 'I', 'M', 'I', 'R' };

// LT7683 SPI cycle type -- bits [7:6] of the first byte of a transaction
// (see LT7683.hpp: CMDWR / DATWR / CMDRD / DATRD)
constexpr std::uint8_t kLtCmdWrite  = 0x00;
constexpr std::uint8_t kLtDataWrite = 0x80;
constexpr std::uint8_t kLtStatus    = 0x40;
constexpr std::uint8_t kLtDataRead  = 0xC0;

constexpr std::uint8_t kRegMrwdp = 0x04;   // memory data read/write port
constexpr std::uint8_t kRegMpwctr = 0x10;  // bit4: which PIP REG[2Ah..3Bh] address
constexpr std::uint8_t kPipRegFirst = 0x2A;
constexpr std::uint8_t kPipRegCount = 0x12;   // 0x2A..0x3B

// CRC-16/CCITT (poly 0x1021, init 0xFFFF), for the frames
inline std::uint16_t crc16(const std::uint8_t* p, std::size_t n, std::uint16_t crc = 0xFFFF) {
    for (std::size_t i = 0; i < n; ++i) {
        crc = static_cast<std::uint16_t>(crc ^ (static_cast<std::uint16_t>(p[i]) << 8));
        for (int b = 0; b < 8; ++b)
            crc = static_cast<std::uint16_t>((crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1);
    }
    return crc;
}

// Builds a frame header (kFrameHeader bytes) for `len` payload bytes
inline void frameHeader(std::uint8_t* h, std::uint8_t sid, std::uint32_t pos,
                        const std::uint8_t* payload, std::size_t len) {
    h[0] = kFrame0; h[1] = kFrame1; h[2] = sid;
    h[3] = static_cast<std::uint8_t>(len); h[4] = static_cast<std::uint8_t>(len >> 8);
    h[5] = static_cast<std::uint8_t>(pos); h[6] = static_cast<std::uint8_t>(pos >> 8);
    h[7] = static_cast<std::uint8_t>(pos >> 16); h[8] = static_cast<std::uint8_t>(pos >> 24);
    const std::uint16_t crc = crc16(payload, len, crc16(h + 2, 7));
    h[9] = static_cast<std::uint8_t>(crc); h[10] = static_cast<std::uint8_t>(crc >> 8);
}

// FNV-1a, for asset ids
inline std::uint32_t fnv1a(const void* data, std::size_t n, std::uint32_t h = 2166136261u) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 16777619u; }
    return h;
}

// Encodes `len` bytes (len even part as 16-bit words) as RLE into `out`
// (capacity at least len + len / 128 + 2). Returns the encoded size.
inline std::size_t rleEncode(const std::uint8_t* in, std::size_t len, std::uint8_t* out) {
    std::size_t o = 0;
    const std::size_t words = len / 2;
    auto word = [&](std::size_t i) { return static_cast<std::uint16_t>(in[2 * i] | (in[2 * i + 1] << 8)); };
    std::size_t i = 0;
    while (i < words) {
        // A run of at least 3 equal words is worth a run token
        std::size_t run = 1;
        while (i + run < words && run < 128 && word(i + run) == word(i)) ++run;
        if (run >= 3) {
            out[o++] = static_cast<std::uint8_t>(0x80 | (run - 1));
            out[o++] = in[2 * i];
            out[o++] = in[2 * i + 1];
            i += run;
            continue;
        }
        // Literals up to the next run of 3 (or 128 words)
        std::size_t lit = 0;
        while (i + lit < words && lit < 128) {
            if (i + lit + 2 < words && word(i + lit) == word(i + lit + 1) &&
                word(i + lit) == word(i + lit + 2)) break;
            ++lit;
        }
        out[o++] = static_cast<std::uint8_t>(lit - 1);
        for (std::size_t k = 0; k < lit; ++k) {
            out[o++] = in[2 * (i + k)];
            out[o++] = in[2 * (i + k) + 1];
        }
        i += lit;
    }
    if (len & 1) out[o++] = in[len - 1];
    return o;
}

}  // namespace mirror
}  // namespace hipi
