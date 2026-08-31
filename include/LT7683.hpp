// SPDX-License-Identifier: MIT
//
// LT7683 (Levetop LT768x family) LCD driver -- mirrors RA8875.hpp's public
// interface exactly (see display_config.h's DisplayDriver alias), so
// Screen.hpp/boardui.cpp/uidialog.hpp etc. work unchanged regardless of
// which chip is actually on the board.
//
// Register-level details are taken directly from the LT768x_DS_ENG V4.0
// datasheet (as supplied) -- specifically:
//   - PLL formula & example (section 6.1 / REG[05h]-[0Ah])
//   - SDRAM init sequence & exact values for LT7683+ (Table 14-6/14-7,
//     REG[E0h]-[E4h])
//   - LCD timing registers & formulas (REG[14h]-[1Fh])
//   - Status register bit layout (Table 14-2)
//   - Geometric drawing engine (REG[67h]-[7Eh])
//   - Block Transfer Engine (REG[90h]-[B4h])
//   - Text engine (REG[CCh]-[D1h]) and colour registers (REG[D2h]-[D7h])
//
// NOT yet confirmed against real hardware:
//   - Everything below is now grounded against either the LT768x datasheet
//     directly OR EastRising's own ESP32 reference firmware for this exact
//     ER-TFTM070-6 module (confirmed to target the same LT768x register
//     map: same REG[05h]-[0Ah] PLL layout, same REG[E0h]-[E4h] SDRAM
//     sequence) -- panel timing (REG[12h]-[1Fh]), PLL targets, and SDRAM
//     refresh interval all come from that reference rather than estimates.
//     Still genuinely unverified: this project's own SPI transport timing
//     against the chip's actual electrical requirements, and anything
//     that can only be confirmed by seeing the panel light up correctly.
//   - The exact DCR1 (REG[76h]) semantics for plain "Draw Rectangle"
//     (bit[5:4]=10b) vs. the DCR0 (REG[67h]) "0010b: Rectangle" option --
//     both exist in this datasheet revision; rect()/fillRect() below use
//     the DCR0 corner-point path (simpler, matches Line/Triangle's own
//     coordinate convention) and leave DCR1's alternate path unused for
//     now.
//   - 2-layer/PIP-equivalent config (RA8875::set2LayerConfig() etc.) --
//     not yet mapped to LT7683's rather different (Picture-in-Picture,
//     multi-buffer) model; stubbed out for now, see the .cpp.

#pragma once

#include "RA8875Transport.hpp"   // same transport interface (SPI byte-level) -- shared, not chip-specific
#include "hp82163_font.hpp"
#include <cstdint>

#define SCREEN_MAX_X        1024
#define SCREEN_MAX_Y        600

namespace hipi {

class LT7683 {
public:
    // -----------------------------------------------------------------------
    // Register addresses -- names match the LT768x datasheet's own register
    // mnemonics so this stays grep-able against it.
    // -----------------------------------------------------------------------

    // SPI command/data bytes (4-wire SPI, per section 7.2's read/write
    // procedure): A0=0/RW#=0 -> write register address; A0=1/RW#=0 -> write
    // data; A0=0/RW#=1 -> read status; A0=1/RW#=1 -> read register data.
    // Encoded the same way RA8875Transport expects (first byte sent over
    // SPI selects the cycle type) -- see LT7683.cpp's writeReg()/readReg().
    static constexpr std::uint8_t CMDWR = 0x00;  // A0=0, RW#=0: register address write
    static constexpr std::uint8_t DATWR = 0x80;  // A0=1, RW#=0: data write
    static constexpr std::uint8_t CMDRD = 0x40;  // A0=0, RW#=1: status read
    static constexpr std::uint8_t DATRD = 0xC0;  // A0=1, RW#=1: data read

    // Configuration registers
    static constexpr std::uint8_t SRR   = 0x00;  // Software Reset Register
    static constexpr std::uint8_t CCR   = 0x01;  // Chip Configuration Register (MCU bus width, LCD bpp)
    static constexpr std::uint8_t MACR  = 0x02;  // Memory Access Control (rotate/mirror, 8bpp mode1/2)
    static constexpr std::uint8_t ICR   = 0x03;  // Input Control Register (text/graphic mode, mem port dest)
    static constexpr std::uint8_t MRWDP = 0x04;  // Memory Data Read/Write Port

    // PLL setting registers (section 6.1) -- three independent PLLs
    static constexpr std::uint8_t PPLLC1 = 0x05;
    static constexpr std::uint8_t PPLLC2 = 0x06;
    static constexpr std::uint8_t MPLLC1 = 0x07;
    static constexpr std::uint8_t MPLLC2 = 0x08;
    static constexpr std::uint8_t CPLLC1 = 0x09;
    static constexpr std::uint8_t CPLLC2 = 0x0A;

    // LCD timing registers (REG[14h]-[1Fh]) -- see the formulas in each
    // setter's comment in the .cpp.
    static constexpr std::uint8_t HDWR   = 0x14;  // Horizontal Display Width
    static constexpr std::uint8_t HDWFTR = 0x15;  // Horizontal Display Width Fine Tune
    static constexpr std::uint8_t HNDR   = 0x16;  // Horizontal Non-Display Period (back porch)
    static constexpr std::uint8_t HNDFTR = 0x17;  // Horizontal Non-Display Period Fine Tune
    static constexpr std::uint8_t HSTR   = 0x18;  // HSYNC Start Position (front porch)
    static constexpr std::uint8_t HPWR   = 0x19;  // HSYNC Pulse Width
    static constexpr std::uint8_t VDHR0  = 0x1A;  // Vertical Display Height low byte
    static constexpr std::uint8_t VDHR1  = 0x1B;  // Vertical Display Height high bits
    static constexpr std::uint8_t VNDR0  = 0x1C;  // Vertical Non-Display Period low byte
    static constexpr std::uint8_t VNDR1  = 0x1D;  // Vertical Non-Display Period high bits
    static constexpr std::uint8_t VSTR   = 0x1E;  // VSYNC Start Position
    static constexpr std::uint8_t VPWR   = 0x1F;  // VSYNC Pulse Width

    // Main window / canvas registers
    static constexpr std::uint8_t MISA0 = 0x20;   // Main Image Start Address
    static constexpr std::uint8_t MIW0  = 0x24;   // Main Image Width

    // Active window registers (REG[56h]-[5Eh])
    static constexpr std::uint8_t AWUL_X0 = 0x56;
    static constexpr std::uint8_t AWUL_Y0 = 0x58;
    static constexpr std::uint8_t AW_WTH0 = 0x5A;
    static constexpr std::uint8_t AW_HT0  = 0x5C;
    static constexpr std::uint8_t AW_COLOR = 0x5E;

    // Graphic read/write cursor position (REG[5Fh]-[62h])
    static constexpr std::uint8_t CURH0 = 0x5F;
    static constexpr std::uint8_t CURV0 = 0x61;
    // Text write cursor position (REG[64h]-[66h])
    static constexpr std::uint8_t F_CURX0 = 0x64;
    static constexpr std::uint8_t F_CURY0 = 0x66;

    // Geometric drawing engine
    static constexpr std::uint8_t DCR0  = 0x67;   // Draw Line/Triangle/Rectangle/... Control 0
    static constexpr std::uint8_t DLHSR0 = 0x68;  // Point 1 X
    static constexpr std::uint8_t DLVSR0 = 0x6A;  // Point 1 Y
    static constexpr std::uint8_t DLHER0 = 0x6C;  // Point 2 X
    static constexpr std::uint8_t DLVER0 = 0x6E;  // Point 2 Y
    static constexpr std::uint8_t DTPH0  = 0x70;  // Triangle Point 3 X
    static constexpr std::uint8_t DTPV0  = 0x72;  // Triangle Point 3 Y
    static constexpr std::uint8_t DCR1  = 0x76;   // Draw Circle/Ellipse/Rounded-Rect Control 1
    static constexpr std::uint8_t ELL_A0 = 0x77;  // Major radius
    static constexpr std::uint8_t ELL_B0 = 0x79;  // Minor radius
    static constexpr std::uint8_t DEHR0  = 0x7B;  // Center X
    static constexpr std::uint8_t DEVR0  = 0x7D;  // Center Y

    // PWM (backlight)
    static constexpr std::uint8_t PSCLR = 0x84;
    static constexpr std::uint8_t PMUXR = 0x85;
    static constexpr std::uint8_t PCFGR = 0x86;

    // Block Transfer Engine (BTE)
    static constexpr std::uint8_t BLT_CTRL0 = 0x90;
    static constexpr std::uint8_t BLT_CTRL1 = 0x91;
    static constexpr std::uint8_t BLT_COLR  = 0x92;
    static constexpr std::uint8_t S0_STR0   = 0x93;  // Source 0 start address (4 bytes)
    static constexpr std::uint8_t S0_WTH0   = 0x97;  // Source 0 image width
    static constexpr std::uint8_t S0_X0     = 0x99;  // Source 0 window X
    static constexpr std::uint8_t S0_Y0     = 0x9B;  // Source 0 window Y (inferred, adjacent to S0_X)
    static constexpr std::uint8_t S1_STR0   = 0x9D;  // Source 1 start address (inferred, mirrors S0 layout)
    static constexpr std::uint8_t S1_WTH0   = 0xA1;
    static constexpr std::uint8_t S1_X0     = 0xA3;
    static constexpr std::uint8_t S1_Y0     = 0xA5;
    static constexpr std::uint8_t DT_STR0   = 0xA7;  // Destination start address
    static constexpr std::uint8_t DT_WTH0   = 0xAB;
    static constexpr std::uint8_t DT_X0     = 0xAD;
    static constexpr std::uint8_t DT_Y0     = 0xAF;
    static constexpr std::uint8_t BLT_WTH0  = 0xB1;  // BTE window width
    static constexpr std::uint8_t BLT_HIG0  = 0xB3;  // BTE window height

    // Text engine
    static constexpr std::uint8_t CCR0_TEXT = 0xCC;  // Character Control 0 (font, size)
    static constexpr std::uint8_t CCR1_TEXT = 0xCD;  // Character Control 1
    static constexpr std::uint8_t FLDR      = 0xD0;  // line gap
    static constexpr std::uint8_t F2FSSR    = 0xD1;  // char-to-char space

    // Colour registers
    static constexpr std::uint8_t FGCR = 0xD2;
    static constexpr std::uint8_t FGCG = 0xD3;
    static constexpr std::uint8_t FGCB = 0xD4;
    static constexpr std::uint8_t BGCR = 0xD5;
    static constexpr std::uint8_t BGCG = 0xD6;
    static constexpr std::uint8_t BGCB = 0xD7;

    // Display RAM (SDRAM) control -- init sequence, see begin()
    static constexpr std::uint8_t SDRAR = 0xE0;
    static constexpr std::uint8_t SDRMD = 0xE1;
    static constexpr std::uint8_t SDR_REF0 = 0xE2;
    static constexpr std::uint8_t SDRCR = 0xE4;

    // Status register bits (Table 14-2)
    static constexpr std::uint8_t STSR_CORE_BUSY   = 0x08;  // bit3: BTE/geometry/DMA/text/graphic busy
    static constexpr std::uint8_t STSR_RAM_READY   = 0x04;  // bit2: Display RAM ready
    static constexpr std::uint8_t STSR_INHIBIT     = 0x02;  // bit1: 1 = inhibited (reset/init/power-save)

    // -----------------------------------------------------------------------
    // Construction / init
    // -----------------------------------------------------------------------
    LT7683(RA8875Transport& t,
           std::uint16_t     width  = SCREEN_MAX_X,
           std::uint16_t     height = SCREEN_MAX_Y,
           bool              start_on = true);

    void begin();
    void begin(const std::uint8_t (*font)[FONT_BYTES_PER_CHAR],
               std::size_t fontCount);

    // -----------------------------------------------------------------------
    // Low-level register/memory access
    // -----------------------------------------------------------------------
    void     writeReg (std::uint8_t cmd, std::uint8_t data);
    void     writeReg (std::uint8_t cmd, const std::uint8_t* data, std::size_t len);
    void     writeReg16(std::uint8_t cmd, std::uint16_t data);
    void     writeCmd (std::uint8_t cmd);
    void     writeData(std::uint8_t data);
    void     writeData(const std::uint8_t* data, std::size_t len);
    std::uint8_t readReg (std::uint8_t cmd);
    std::uint8_t readData();
    std::uint8_t readStatus();

    void waitPoll (std::uint8_t reg, std::uint8_t mask);
    void waitStatus(std::uint8_t mask = STSR_CORE_BUSY);

    void spiDelayMs(std::uint32_t ms) { t_.delayMs(ms); }

    // -----------------------------------------------------------------------
    // Block Transfer Engine -- see terminal.cpp/boardui.cpp's button-strip
    // slide animation for the hard-won lesson on Positive vs Negative
    // direction move opcodes with overlapping source/destination. NOTE:
    // per this datasheet's section 12.3.2, LT768x's "Memory Copy with ROP"
    // (opcode 0010b) explicitly "supports data transfer in positive
    // direction only" -- there is no Negative Direction equivalent here,
    // unlike RA8875. Any animation relying on that trick (the hide
    // animation's rightward, overlapping shift) needs a different approach
    // on this chip -- e.g. going through the same "SPI-redraw per step"
    // fallback boardui.cpp already has a precedent for.
    // -----------------------------------------------------------------------
    void BTE(std::uint8_t  opcode,
             std::uint16_t x1,  std::uint16_t y1,
             std::uint16_t w,   std::uint16_t h,
             std::uint16_t x0 = 0, std::uint16_t y0 = 0);

    // -----------------------------------------------------------------------
    // Power / reset / backlight
    // -----------------------------------------------------------------------
    void reset();
    void softReset();
    void turnOn(bool on);
    void sleep(bool s);
    void brightness(std::uint8_t level);

    void pllInit();

    // -----------------------------------------------------------------------
    // Mode
    // -----------------------------------------------------------------------
    void gfxMode();
    void txtMode();

    // -----------------------------------------------------------------------
    // Colour
    // -----------------------------------------------------------------------
    void setColor  (std::uint16_t c);
    void setBgColor(std::uint16_t c);

    // -----------------------------------------------------------------------
    // Text
    // -----------------------------------------------------------------------
    void setxy   (std::uint16_t x, std::uint16_t y);
    void txtSetCursor(std::uint16_t x, std::uint16_t y);
    void txtColor(std::uint16_t fg, std::uint16_t bg);
    void txtTrans(std::uint16_t color);
    void txtSize (std::uint8_t scale);
    void txtWrite(const char* s);
    void txtWriteChar(std::uint8_t c);

    // -----------------------------------------------------------------------
    // Graphics primitives (hardware-accelerated via the geometric drawing
    // engine, REG[67h]-[7Eh] -- see LT7683.cpp)
    // -----------------------------------------------------------------------
    void pixel   (std::int16_t x, std::int16_t y, std::uint16_t color);
    void fillRect(std::int16_t x, std::int16_t y, std::int16_t w, std::int16_t h, std::uint16_t color);
    void clearActiveWindow();
    void beginMemoryWrite();
    void rect    (std::int16_t x, std::int16_t y, std::int16_t w, std::int16_t h, std::uint16_t color);
    void fillRoundRect(std::int16_t x, std::int16_t y, std::int16_t w, std::int16_t h,
                        std::uint16_t r, std::uint16_t color);
    void fill    (std::uint16_t color);
    void hline   (std::int16_t x, std::int16_t y, std::int16_t w, std::uint16_t color);
    void vline   (std::int16_t x, std::int16_t y, std::int16_t h, std::uint16_t color);
    void line    (std::int16_t x1, std::int16_t y1, std::int16_t x2, std::int16_t y2, std::uint16_t color);
    void drawBitmap565(std::int16_t x, std::int16_t y,
                    std::uint16_t w, std::uint16_t h,
                    const std::uint16_t* data);
    void drawBitmap565Cropped(std::int16_t x, std::int16_t y,
                    std::uint16_t drawWidth, std::uint16_t h,
                    std::uint16_t srcStride,
                    const std::uint16_t* data);
    void drawBitmap332(std::int16_t x, std::int16_t y,
                    std::uint16_t w, std::uint16_t h,
                    const std::uint8_t* data);
    void setActiveWindow(std::uint16_t x0, std::uint16_t y0,
                     std::uint16_t x1, std::uint16_t y1);

    // -----------------------------------------------------------------------
    // Colour depth / layers -- LT7683's model (multi-buffer + PIP) doesn't
    // map cleanly onto RA8875's simple 2-layer overlay. Stubbed for now
    // (log a "not implemented" note, don't touch hardware state) until
    // it's clear whether the project actually needs the equivalent
    // capability on this chip -- see LT7683.cpp.
    // -----------------------------------------------------------------------
    void set8Bpp();
    void set2LayerConfig();

    // -----------------------------------------------------------------------
    // CGRAM custom characters -- LT7683 has larger built-in CGROM fonts
    // (8x16/12x24/16x32) than RA8875. Deferred: see if the project can just
    // use a built-in size instead of uploading the HP-41 custom glyph set,
    // before implementing LT7683's own (differently-organised) UCG upload.
    // -----------------------------------------------------------------------
    void uploadCgramChar(std::uint8_t ascii, const std::uint8_t bitmap[16]);

    // -----------------------------------------------------------------------
    // Accessors
    // -----------------------------------------------------------------------
    std::uint16_t width()      const { return width_; }
    std::uint16_t height()     const { return height_; }
    std::uint16_t vertOffset() const { return vertOffset_; }
    std::uint8_t  txtScale()   const { return txtScale_; }

private:
    void rectHelper (std::int16_t x1, std::int16_t y1, std::int16_t x2, std::int16_t y2,
                     std::uint16_t color, bool filled);
    void circleHelper(std::int16_t x, std::int16_t y, std::uint16_t r, std::uint16_t color, bool filled);
    void ellipseHelper(std::int16_t x, std::int16_t y, std::uint16_t ha, std::uint16_t va,
                       std::uint16_t color, bool filled);
    void roundRectHelper(std::int16_t x1, std::int16_t y1, std::int16_t x2, std::int16_t y2,
                         std::uint16_t rr, std::uint16_t color, bool filled);

    RA8875Transport& t_;
    std::uint16_t width_;
    std::uint16_t height_;
    std::uint16_t vertOffset_ = 0;
    std::uint8_t  txtScale_ = 0;
};

}  // namespace hipi
