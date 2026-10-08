// mirror_session.hpp -- starts a display mirror session for a newly
// connected viewer and brings it in step with the real display: greeting,
// register snapshot, built-in font, then the SDRAM read-back in small
// steps (see display_mirror.h for the whole picture).
//
// What is read back, in this order (what's visible first):
//   1. the panel itself
//   2. what the PIP windows show right now (an open menu, the buttons)
//   3. the registered layers that change at run time (button strip,
//      cassette label, ...), and the CGRAM font
//   4. the registered assets -- layers filled once from a fixed source,
//      e.g. the Tape view pictures. The viewer caches these; those it
//      already has (it lists them in its request) are not read back at
//      all, just named (kAssetUse).
//
// Header-only and free of Pico SDK dependencies, so tools/hipiview's host
// test runs exactly this code against an emulated chip.
#pragma once
#include "LT7683.hpp"
#include "mirror_encoder.hpp"
#include <algorithm>
#include <cstring>
#include <new>

namespace hipi {
namespace mirror {

// A layer (or part of one) worth sending: rows x rowBytes from addr,
// rowStep bytes apart. assetId != 0: filled once from a fixed source.
struct Layer {
    std::uint32_t addr = 0, rowBytes = 0, rowStep = 0;
    std::uint16_t rows = 0;
    std::uint32_t assetId = 0;
};

class Session {
public:
    explicit Session(Encoder& enc) : enc_(enc) {}

    // ── Registry (filled at boot by whoever loads a layer) ──────────────
    // Replaces an earlier entry for the same address.
    void addLayer(const Layer& l) {
        for (int i = 0; i < layerCount_; ++i)
            if (layers_[i].addr == l.addr) { layers_[i] = l; return; }
        if (layerCount_ < kMaxLayers) layers_[layerCount_++] = l;
    }

    // The asset ids the viewer has cached (from its request)
    void setViewerAssets(const std::uint32_t* ids, int n) {
        viewerAssetCount_ = std::min(n, kMaxViewerAssets);
        for (int i = 0; i < viewerAssetCount_; ++i) viewerAssets_[i] = ids[i];
    }

    // Sends greeting, register snapshot and font, and plans the read-back.
    // The encoder must already be enabled.
    void start(LT7683& d) {
        std::uint8_t hello[1 + 7 + 6];
        hello[0] = kHello;
        std::memcpy(hello + 1, kMagic, 7);
        hello[8] = kVersion;
        hello[9] = kChipLT7683;
        hello[10] = static_cast<std::uint8_t>(d.width() & 0xFF);
        hello[11] = static_cast<std::uint8_t>(d.width() >> 8);
        hello[12] = static_cast<std::uint8_t>(d.height() & 0xFF);
        hello[13] = static_cast<std::uint8_t>(d.height() >> 8);
        enc_.emit(hello, sizeof(hello));

        // Register snapshot: the values last written (the encoder keeps
        // them all the time), except the cursors, which the chip moves by
        // itself while drawing -- those are read back
        static constexpr std::uint8_t kLive[] = { 0x5F, 0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66 };
        std::uint8_t live[sizeof(kLive)];
        enc_.suspend();
        for (std::size_t i = 0; i < sizeof(kLive); ++i) live[i] = d.readReg(kLive[i]);
        enc_.resume();
        for (int r = 0; r < 256; ++r) {
            if (r == kRegMrwdp) continue;
            if (r >= kPipRegFirst && r < kPipRegFirst + kPipRegCount) continue;
            std::uint8_t v = enc_.reg(static_cast<std::uint8_t>(r));
            for (std::size_t i = 0; i < sizeof(kLive); ++i) if (kLive[i] == r) v = live[i];
            const std::uint8_t p[3] = { kSetReg, static_cast<std::uint8_t>(r), v };
            enc_.emit(p, 3);
        }
        for (int set = 0; set < 2; ++set)
            for (int i = 0; i < kPipRegCount; ++i) {
                const std::uint8_t p[4] = { kSetPip, static_cast<std::uint8_t>(set),
                                            static_cast<std::uint8_t>(i), enc_.pip(set, i) };
                enc_.emit(p, 4);
            }
        const std::uint8_t cur[2] = { kSetCur, enc_.curReg() };
        enc_.emit(cur, 2);

        // The built-in font -- captured once (drawn off-screen and read
        // back, unseen by the viewer)
        if (font_ == nullptr) {
            font_ = new (std::nothrow) std::uint8_t[256][16];
            if (font_ != nullptr) {
                enc_.suspend();
                d.captureBuiltinFont(font_);
                enc_.resume();
            }
        }
        if (font_ != nullptr) {
            const std::uint8_t op = kFont;
            enc_.emit(&op, 1);
            enc_.emit(&font_[0][0], 256 * 16);
        }

        planDump();
        std::uint32_t total = 0;
        for (int i = 0; i < itemCount_; ++i) total += items_[i].layer.rowBytes * items_[i].layer.rows;
        const std::uint8_t start[5] = { kSyncStart,
            static_cast<std::uint8_t>(total), static_cast<std::uint8_t>(total >> 8),
            static_cast<std::uint8_t>(total >> 16), static_cast<std::uint8_t>(total >> 24) };
        enc_.emit(start, sizeof(start));
        enc_.flush();             // the greeting goes out at once
        done_ = false;
    }

    // Reads the next piece (about `budget` bytes) of the planned SDRAM
    // areas and sends it as kMem. Returns true once everything is sent.
    bool step(LT7683& d, std::size_t budget = 8192) {
        if (done_) return true;
        static std::uint8_t buf[kChunk];
        while (budget > 0 && item_ < itemCount_) {
            const Item& it = items_[item_];
            const Layer& r = it.layer;
            if (row_ == 0 && offset_ == 0 && r.assetId != 0) emitAsset(kAssetBegin, r);
            const std::uint32_t left = r.rowBytes - offset_;
            const std::size_t n = std::min<std::size_t>(std::min<std::size_t>(left, kChunk), budget);
            const std::uint32_t addr = r.addr + static_cast<std::uint32_t>(row_) * r.rowStep + offset_;
            enc_.suspend();
            d.readSdram(addr, buf, n);
            enc_.resume();
            enc_.emitMem(addr, buf, n);
            if (!enc_.enabled()) return false;         // overflow meanwhile
            budget -= std::min(budget, n);
            offset_ += static_cast<std::uint32_t>(n);
            if (offset_ >= r.rowBytes) {
                offset_ = 0;
                if (++row_ >= r.rows) {
                    row_ = 0;
                    if (r.assetId != 0) {
                        const std::uint8_t p[5] = { kAssetEnd,
                            static_cast<std::uint8_t>(r.assetId), static_cast<std::uint8_t>(r.assetId >> 8),
                            static_cast<std::uint8_t>(r.assetId >> 16), static_cast<std::uint8_t>(r.assetId >> 24) };
                        enc_.emit(p, sizeof(p));
                    }
                    ++item_;
                }
            }
        }
        if (item_ >= itemCount_) {
            const std::uint8_t op = kSyncDone;
            enc_.emit(&op, 1);
            done_ = true;
        }
        return done_;
    }

private:
    static constexpr std::size_t kChunk = 2048;
    static constexpr int kMaxLayers = 12;
    static constexpr int kMaxItems = kMaxLayers + 6;
    struct Item { Layer layer; };

    bool viewerHas(std::uint32_t id) const {
        for (int i = 0; i < viewerAssetCount_; ++i) if (viewerAssets_[i] == id) return true;
        return false;
    }
    void addItem(const Layer& l) {
        if (itemCount_ < kMaxItems && l.rowBytes > 0 && l.rows > 0) items_[itemCount_++] = Item{ l };
    }
    void emitAsset(std::uint8_t op, const Layer& l) {
        const std::uint8_t p[19] = { op,
            static_cast<std::uint8_t>(l.assetId), static_cast<std::uint8_t>(l.assetId >> 8),
            static_cast<std::uint8_t>(l.assetId >> 16), static_cast<std::uint8_t>(l.assetId >> 24),
            static_cast<std::uint8_t>(l.addr), static_cast<std::uint8_t>(l.addr >> 8),
            static_cast<std::uint8_t>(l.addr >> 16), static_cast<std::uint8_t>(l.addr >> 24),
            static_cast<std::uint8_t>(l.rowBytes), static_cast<std::uint8_t>(l.rowBytes >> 8),
            static_cast<std::uint8_t>(l.rowBytes >> 16), static_cast<std::uint8_t>(l.rowBytes >> 24),
            static_cast<std::uint8_t>(l.rowStep), static_cast<std::uint8_t>(l.rowStep >> 8),
            static_cast<std::uint8_t>(l.rowStep >> 16), static_cast<std::uint8_t>(l.rowStep >> 24),
            static_cast<std::uint8_t>(l.rows), static_cast<std::uint8_t>(l.rows >> 8) };
        enc_.emit(p, sizeof(p));
    }
    std::uint16_t pipWord(int set, int idx) const {
        return static_cast<std::uint16_t>(enc_.pip(set, idx) | (enc_.pip(set, idx + 1) << 8));
    }
    void planDump() {
        const std::uint32_t row = 1024UL * 2UL;
        itemCount_ = 0;
        // 1. The panel
        Layer panel;
        panel.addr = 0; panel.rowBytes = row * 600; panel.rowStep = 0; panel.rows = 1;
        addItem(panel);
        // 2. What the PIP windows show right now
        const std::uint8_t mpwctr = enc_.reg(kRegMpwctr);
        for (int set = 0; set < 2; ++set) {
            if (!(mpwctr & (set == 0 ? 0x80 : 0x40))) continue;
            Layer l;
            const std::uint32_t addr = static_cast<std::uint32_t>(pipWord(set, 4)) |
                                       (static_cast<std::uint32_t>(pipWord(set, 6)) << 16);
            const std::uint16_t stride = pipWord(set, 8);
            l.addr = addr + (static_cast<std::uint32_t>(pipWord(set, 12)) * stride + pipWord(set, 10)) * 2;
            l.rowBytes = static_cast<std::uint32_t>(pipWord(set, 14)) * 2;
            l.rowStep = static_cast<std::uint32_t>(stride) * 2;
            l.rows = pipWord(set, 16);
            addItem(l);
        }
        // 3. Layers that change at run time, and the CGRAM font
        for (int i = 0; i < layerCount_; ++i) if (layers_[i].assetId == 0) addItem(layers_[i]);
        Layer cgram;
        cgram.addr = LT7683::kCgramAddr; cgram.rowBytes = 256 * 16; cgram.rows = 1;
        addItem(cgram);
        // 4. Assets: named if the viewer has them, else read back (once)
        for (int i = 0; i < layerCount_; ++i) {
            const Layer& l = layers_[i];
            if (l.assetId == 0) continue;
            if (viewerHas(l.assetId)) emitAsset(kAssetUse, l);
            else addItem(l);
        }
        item_ = 0;
        row_ = 0;
        offset_ = 0;
    }

    Encoder& enc_;
    std::uint8_t (*font_)[16] = nullptr;      // captured once, kept (4 kB)
    Layer layers_[kMaxLayers] = {};
    int layerCount_ = 0;
    std::uint32_t viewerAssets_[kMaxViewerAssets] = {};
    int viewerAssetCount_ = 0;
    Item items_[kMaxItems] = {};
    int itemCount_ = 0;
    int item_ = 0;                // the one being read
    std::uint16_t row_ = 0;       // its row
    std::uint32_t offset_ = 0;    // position in that row
    bool done_ = true;
};

}  // namespace mirror
}  // namespace hipi
