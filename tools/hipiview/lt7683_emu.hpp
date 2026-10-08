// lt7683_emu.hpp -- an emulated LT7683 display controller, as far as
// HIPI uses it: SDRAM with layers, memory writes/reads (block and linear
// addressing), the text engine (built-in font and user-defined CGRAM
// characters, scaling, transparency, spacing), the geometry engine
// (lines, rectangles, rounded rectangles, ellipses, triangles), the BTE
// (memory copy and MCU write with ROP, solid fill), the main window,
// PIP-1/PIP-2, the blinking text cursor, display on/off and the PWM
// backlight.
//
// Two ways in:
//   feed()             the display mirror stream (mirror_protocol.h) --
//                      what hipiview uses
//   hostCmd()/hostWrite()/hostRead()/hostStatus()
//                      raw SPI cycles, answering reads like the chip --
//                      lets the host test drive the real LT7683 driver
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class LT7683Emu {
public:
    LT7683Emu();
    void reset();

    // ── Mirror stream ───────────────────────────────────────────────────
    void feed(const std::uint8_t* data, std::size_t n);
    // The stream was damaged: ignore everything until the next greeting
    void desync() { hello_ = false; syncing_ = false; pending_.clear(); }

    // The asset cache (layer pictures sent only once, see mirror_protocol.h):
    // load(id, bytes) fills `bytes` and returns true if the asset is cached;
    // save(id, bytes) keeps a newly received one.
    std::function<bool(std::uint32_t, std::vector<std::uint8_t>&)> loadAsset;
    std::function<void(std::uint32_t, const std::vector<std::uint8_t>&)> saveAsset;

    // ── Raw SPI cycles (host test) ──────────────────────────────────────
    void hostCmd(std::uint8_t reg) { cmd(reg, true); }
    void hostWrite(std::uint8_t v) { dataWrite(v); }
    std::uint8_t hostRead();
    std::uint8_t hostStatus() const { return 0x44; }   // RAM ready, write FIFO empty, idle
    void setBuiltinFont(const std::uint8_t font[256][16]);

    // ── Output ──────────────────────────────────────────────────────────
    int width() const { return width_; }
    int height() const { return height_; }
    // The panel as shown: main window, PIP windows, text cursor; dimmed by
    // the backlight unless backlight = false. out: width*height 0xAARRGGBB.
    void render(std::uint32_t* out, double timeSec, bool backlight = true) const;

    // ── Status ──────────────────────────────────────────────────────────
    bool connected() const { return hello_; }       // a session has started
    bool syncing() const { return syncing_; }
    int syncPercent() const;
    bool fontKnown() const { return fontKnown_; }
    std::uint64_t streamErrors() const { return errors_; }
    const std::vector<std::uint8_t>& memory() const { return mem_; }
    std::uint8_t reg(std::uint8_t r) const { return regs_[r]; }

private:
    // Stream parser
    std::size_t parse(const std::uint8_t* p, std::size_t n);   // bytes used, 0 = need more
    std::vector<std::uint8_t> pending_;

    // Chip
    void cmd(std::uint8_t reg, bool real);
    void dataWrite(std::uint8_t v);
    void dataRead(std::uint8_t v);                     // a read the real chip answered
    void setReg(std::uint8_t r, std::uint8_t v);       // no side effects
    void writeReg(std::uint8_t r, std::uint8_t v);     // with side effects
    void memWrite(std::uint8_t v);
    std::uint8_t memRead(bool apply, std::uint8_t v);
    void textByte(std::uint8_t v);
    void drawChar(std::uint32_t code);
    void dcr0(std::uint8_t v);
    void dcr1(std::uint8_t v);
    void bteStart();
    void bteMcuPixel(std::uint16_t s0);

    std::uint16_t r16(int a) const { return static_cast<std::uint16_t>(regs_[a] | (regs_[a + 1] << 8)); }
    std::uint32_t r32(int a) const { return static_cast<std::uint32_t>(r16(a)) | (static_cast<std::uint32_t>(r16(a + 2)) << 16); }
    void w16(int a, std::uint32_t v) { regs_[a] = static_cast<std::uint8_t>(v); regs_[a + 1] = static_cast<std::uint8_t>(v >> 8); }
    std::uint16_t p16(int set, int i) const { return static_cast<std::uint16_t>(pip_[set][i] | (pip_[set][i + 1] << 8)); }

    std::uint16_t pix(std::uint32_t addr) const;
    void setPix(std::uint32_t addr, std::uint16_t c);
    // Canvas drawing, clipped to the Active Window
    void plot(int x, int y, std::uint16_t c);
    void span(int x0, int x1, int y, std::uint16_t c);
    void line(int x0, int y0, int x1, int y1, std::uint16_t c);
    void ellipse(int cx, int cy, int a, int b, bool fill, std::uint16_t c);
    void roundRect(int x0, int y0, int x1, int y1, int a, int b, bool fill, std::uint16_t c);
    void triangle(int x0, int y0, int x1, int y1, int x2, int y2, bool fill, std::uint16_t c);
    std::uint16_t fg() const;
    std::uint16_t bg() const;

    int width_ = 1024, height_ = 600;
    std::vector<std::uint8_t> mem_;
    std::uint8_t regs_[256];
    std::uint8_t pip_[2][0x12];
    std::uint8_t cur_ = 0;
    bool dummy_ = false;
    int wrLo_ = -1, rdLo_ = -1;       // first byte of a 16-bit pixel
    int ucgHi_ = -1;                  // first byte of a 2-byte character code
    bool bteMcu_ = false;
    std::uint32_t bteCount_ = 0;
    std::uint8_t font_[256][16];
    bool fontKnown_ = false;

    bool hello_ = false;
    bool syncing_ = false;
    std::uint32_t syncTotal_ = 0, syncDone_ = 0;
    struct AssetArea { std::uint32_t id, addr, rowBytes, rowStep; std::uint16_t rows; };
    std::vector<AssetArea> assetsBegun_;
    void memBytes(std::uint32_t addr, const std::uint8_t* p, std::size_t n);
    std::uint64_t errors_ = 0;
};
