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
//   - DCR1 (REG[76h]) IS used, for circle/ellipse/rounded-rectangle
//     (bit[5:4]=11b) -- rect()/fillRect() use DCR0's own "0010b:
//     Rectangle" option instead for plain rectangles (simpler, matches
//     Line/Triangle's own coordinate convention), leaving DCR1's
//     alternate plain-rectangle option (bit[5:4]=10b) unused. One
//     DCR1 constraint confirmed the hard way on real hardware: section
//     6.6's Note1/Note2 require the bounding width/height to be
//     STRICTLY GREATER than 2*radius+1, not merely >=2*radius --
//     violating this by exactly 1 (e.g. a 20x20 circle via radius=10)
//     produced a corrupted draw (a long streak to the bottom of the
//     panel) instead of a clipped/adjusted shape. fillRoundRect()/
//     roundRect() clamp for this correctly now -- see their own comments.
//   - 2-layer/PIP-equivalent config (RA8875::set2LayerConfig() etc.) --
//     not yet mapped to LT7683's rather different (Picture-in-Picture,
//     multi-buffer) model; stubbed out for now, see the .cpp.

#pragma once

#include "RA8875Transport.hpp"   // same transport interface (SPI byte-level) -- shared, not chip-specific
#include "hp82163_font.hpp"
#include <cstdint>
#include <cstdio>

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
    static constexpr std::uint8_t MWSXY0 = 0x26;  // Main Window Start X/Y (pan offset within Main Image)
    static constexpr std::uint8_t CVSSA0 = 0x50;  // Canvas Image Start Address -- the drawing
                                                   // engine's own target buffer; SEPARATE from
                                                   // Main Image Start Address (what's actually
                                                   // scanned out to the panel). Both must point
                                                   // at the same place for anything drawn to
                                                   // become visible -- see begin()'s comment.
    static constexpr std::uint8_t CVS_IMWTH0 = 0x54;  // Canvas Image Width (drawing engine's stride)

    // PIP (Picture-in-Picture) registers -- see beginOverlayDraw()'s own
    // comment for the full story of how these are used to show the menu
    // without ever pausing the main window's own live updates.
    static constexpr std::uint8_t MPWCTR  = 0x10;  // Main/PIP Window Control -- PIP-1/2 enable bits,
                                                    // and which of PIP-1/2 the registers below
                                                    // configure (bit4: 0=PIP-1, 1=PIP-2)
    static constexpr std::uint8_t PIPCDEP = 0x11;  // PIP-1/2 Window Color Depth
    static constexpr std::uint8_t PWDULX0 = 0x2A;  // PIP Window Display Upper-Left X (on-screen
                                                    // position) -- bit[1:0] must be 0 (4-pixel unit)
    static constexpr std::uint8_t PWDULY0 = 0x2C;  // PIP Window Display Upper-Left Y (on-screen)
    static constexpr std::uint8_t PISA0   = 0x2E;  // PIP Image Start Address (4 bytes) -- bit[1:0]
                                                    // of the low byte must be 0
    static constexpr std::uint8_t PIW0    = 0x32;  // PIP Image Width (source layer's own stride) --
                                                    // bit[1:0] must be 0
    static constexpr std::uint8_t PWIULX0 = 0x34;  // PIP Window Image Upper-Left X (where within
                                                    // the source image to start reading) --
                                                    // bit[1:0] must be 0
    static constexpr std::uint8_t PWIULY0 = 0x36;  // PIP Window Image Upper-Left Y
    static constexpr std::uint8_t PWW0    = 0x38;  // PIP Window Width (how wide the on-screen
                                                    // overlay is) -- bit[1:0] must be 0
    static constexpr std::uint8_t PWH0    = 0x3A;  // PIP Window Height

    // Active window registers (REG[56h]-[5Eh])
    static constexpr std::uint8_t AWUL_X0 = 0x56;
    static constexpr std::uint8_t AWUL_Y0 = 0x58;
    static constexpr std::uint8_t AW_WTH0 = 0x5A;
    static constexpr std::uint8_t AW_HT0  = 0x5C;
    static constexpr std::uint8_t AW_COLOR = 0x5E;

    // Graphic read/write cursor position (REG[5Fh]-[62h])
    static constexpr std::uint8_t CURH0 = 0x5F;
    static constexpr std::uint8_t CURV0 = 0x61;
    // Text write cursor position (REG[63h]-[66h]) -- confirmed against
    // EastRising's Goto_Text_XY(): X low/high = REG[63h]/[64h], Y
    // low/high = REG[65h]/[66h]. An earlier pass here had these off by
    // one byte (F_CURX0=0x64, F_CURY0=0x66 -- X's *high* byte, and Y's
    // low byte respectively), which sent every text cursor position to
    // the wrong register pair entirely -- text was being written, just
    // never at a visible/sane position on screen.
    static constexpr std::uint8_t F_CURX0 = 0x63;
    static constexpr std::uint8_t F_CURY0 = 0x65;

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
    // CGRAM Start Address (CGRAM_STR0) -- REG[DBh]-[DEh], a 32-bit
    // address (little-endian, same 4-consecutive-registers layout as
    // CVSSA0). Confirmed against the datasheet's own register table
    // (section 14.10 -- the page itself has a typo, listing REG[DEh]
    // twice for bits [23:16] AND [31:24], but the standard consecutive
    // pattern DBh/DCh/DDh/DEh matches every other multi-byte register
    // in this file, CVSSA0 included). Tells the TEXT ENGINE where
    // user-defined characters (UCG/CGRAM) actually live -- separate
    // from CVSSA, which is just the drawing engine's own read/write
    // pointer used WHILE uploading them. The datasheet's own words:
    // "Host must use canvas image setting to organize CGRAM data and
    // set CGRAM address to tell engine where to fetch CGRAM data" --
    // confirms this must be set explicitly; it doesn't default to
    // wherever CVSSA last pointed during upload. See begin()'s own
    // setting of this to kCgramAddr, once, right after the font upload
    // loop.
    static constexpr std::uint8_t CGRAM_STR0 = 0xDB;
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
    static constexpr std::uint8_t STSR_WR_FIFO_FULL = 0x80; // bit7: Memory Write FIFO full -- writeData()'s
                                                              // bulk overload never checked this; drawBitmap565Cropped()
                                                              // now does, see its own comment.
    // bit6: Memory Write FIFO empty -- 0=not empty, 1=empty. Confirmed
    // against a working reference implementation of this exact chip
    // (ESP32-based, MPU8_8bpp_Memory_Write and friends): the correct
    // "all writes have actually landed" check after a multi-byte MRWDP
    // write is THIS bit, not STSR_CORE_BUSY -- see uploadCgramChar()'s
    // own comment.
    static constexpr std::uint8_t STSR_WR_FIFO_EMPTY = 0x40;

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
    void     writeReg32(std::uint8_t cmd, std::uint32_t data);
    void     writeCmd (std::uint8_t cmd);
    void     writeData(std::uint8_t data);
    void     writeData(const std::uint8_t* data, std::size_t len);
    std::uint8_t readReg (std::uint8_t cmd);
    std::uint8_t readData();
    // Multi-byte counterpart to writeData(data*, len) -- reads `len`
    // bytes into `buf` within a SINGLE CS-low/CS-high SPI transaction
    // (one DATRD prefix byte, then `len` data bytes read back-to-back),
    // rather than `len` separate single-byte transactions. This matters
    // for auto-incrementing memory reads (MRWDP -- see verifyCgramChar()'s
    // own comment): confirmed on real hardware that reading via repeated
    // single-byte readData() calls (each its own CS toggle) does NOT
    // correctly continue reading from where the previous byte left off,
    // while a single continuous transaction (matching how writeData()'s
    // own multi-byte overload already works, and how the datasheet's
    // own "Read Register's Data" procedure describes CS staying low for
    // the whole exchange) does.
    // Multi-byte read -- reads `len` bytes starting at register `cmd`,
    // incrementing `cmd` once per byte (writeCmd(cmd++) then the
    // single-byte readData() above, each its own full CS-toggle
    // transaction -- exactly like readReg()'s own proven-working
    // pattern, just repeated `len` times). Confirmed on real hardware
    // as the reliable way to read back BOTH a sequential multi-byte
    // register (e.g. CVSSA0..CVSSA0+3, 4 distinct register addresses)
    // AND MRWDP's own auto-incrementing Canvas data (`cmd` itself
    // increments too there, but the chip's own DATRD response keeps
    // tracking the Canvas pointer regardless of the specific register
    // address last selected) -- a single continuous multi-byte SPI
    // transaction (this function's own earlier design) was tried first
    // and confirmed NOT to work reliably for this chip's own read path,
    // even though the equivalent single continuous transaction works
    // fine for writes (see writeData(data*, len)).
    void readData(std::uint8_t cmd, std::uint8_t* buf, std::size_t len);
    std::uint8_t readStatus();

    void waitPoll (std::uint8_t reg, std::uint8_t mask);
    void waitStatus(std::uint8_t mask = STSR_CORE_BUSY);
    // Opposite polarity from waitStatus(): waits until the given
    // status bit(s) become SET (waitStatus() waits until CLEAR).
    // Needed for STSR_WR_FIFO_EMPTY, which is "1 = empty" (unlike
    // STSR_CORE_BUSY/STSR_WR_FIFO_FULL, both "1 = busy/full").
    void waitStatusSet(std::uint8_t mask);

    void spiDelayMs(std::uint32_t ms) { t_.delayMs(ms); }

    // -----------------------------------------------------------------------
    // Block Transfer Engine.
    //
    // STATUS: still unverified/likely broken on real hardware, despite
    // extensive bring-up testing. Confirmed correct along the way: ROP
    // codes (0xC2/0xC3, matching RA8875's own convention, both exist and
    // work the same shape here -- an earlier note claiming only
    // positive-direction moves were supported was wrong, based on one
    // misread datasheet line), REG[92h] (BLT_COLR, S0/S1/Destination
    // colour depth -- missing entirely at first, now set), the trigger/
    // wait mechanism (tried both blind and read-modify-write REG[90h]
    // writes, both REG[90h] and the dedicated STATUSREAD command for
    // polling busy), several S1 register configurations (matching source,
    // matching destination, all zero, left untouched -- per different
    // real reference implementations), active window, Main Image/Canvas
    // addressing, and Display Off/On cycling. All confirmed correct or
    // healthy right up to and after the trigger -- yet the panel still
    // ends up showing solid colour that survives even a Display Off/On
    // toggle (i.e. SDRAM itself keeps getting overwritten, not just a
    // frozen screen). That's beyond what register-level testing from
    // here can diagnose further; needs real hardware inspection (e.g. an
    // oscilloscope on SDRAM signals during a BTE op) to make progress.
    //
    // Until then: LT7683::clearActiveWindow() no longer uses this (see
    // its own comment -- converted to the geometry engine instead, since
    // Screen::clear()/full() call it on every construction/scroll and
    // that was freezing the display before any text could be drawn).
    // boardui.cpp's button-strip slide animation and Screen::bte()'s
    // scroll/insert/delete-line handling still call this directly and
    // remain unconverted -- see display_boot_test.hpp's own file-header
    // note for the full list.
    // -----------------------------------------------------------------------
    void BTE(std::uint8_t  opcode,
             std::uint16_t x1,  std::uint16_t y1,
             std::uint16_t w,   std::uint16_t h,
             std::uint16_t x0 = 0, std::uint16_t y0 = 0);

    // See its own file-header comment in the .cpp for the full story --
    // this is the actual, working BTE implementation (BTE_MCU_Write with
    // ROP, per the manufacturer's own Application Note), meant as a
    // faster alternative to drawBitmap565Cropped() for exactly the same
    // signature/use case (the button strip's own sliding-bitmap
    // animation, drawn via plain MRWDP so far).
    void bteMcuWriteBitmap(std::int16_t x, std::int16_t y,
                           std::uint16_t drawWidth, std::uint16_t h,
                           std::uint16_t srcStride,
                           const std::uint16_t* data);
    // Low-level BTE "Memory Copy with ROP" (opcode 0010b, ROP 0xC --
    // "DT = S0", same reasoning as bteMcuWriteBitmap()'s own ROP choice)
    // -- moves a w x h pixel region from one SDRAM address/window to
    // another, entirely within the chip, no SPI pixel data at all. NOT
    // safe to call directly with source and destination regions that
    // overlap in memory -- see bteScrollShift()'s own comment for why,
    // and for the safe way to do an overlapping (in-place) shift.
    // Unlike most other drawing calls in this class, this does NOT
    // auto-apply vertOffset_ to srcY/dstY -- srcAddr/dstAddr can be
    // arbitrary SDRAM layers (not just the live, address-0 panel this
    // project's own vertical offset quirk applies to), so callers need
    // to add it themselves for whichever of the two actually lands on
    // the panel (see bteScrollShift()'s own use for an example).
    void bteMemoryCopy(std::uint32_t srcAddr, std::uint16_t srcStride,
                       std::int16_t srcX, std::int16_t srcY,
                       std::uint32_t dstAddr, std::uint16_t dstStride,
                       std::int16_t dstX, std::int16_t dstY,
                       std::uint16_t w, std::uint16_t h);
    // Shifts a w x h region UP by shiftRows pixel rows, entirely via BTE
    // (no SPI pixel data), safely handling the fact that source and
    // destination overlap when done in-place (moving content up within
    // the SAME on-screen region: destination Y is LOWER than source Y,
    // but by less than the full region height, so they cover common
    // ground). Neither this chip's datasheet nor the manufacturer's own
    // Application Note documents how (or whether) a single Memory Copy
    // call handles that overlap safely on its own -- rather than risk
    // it (BTE has already been confirmed, once, to corrupt/freeze this
    // exact panel from a different addressing mistake -- see
    // bteMcuWriteBitmap()'s own file-header comment), this goes via an
    // intermediate staging copy in a third, entirely separate SDRAM
    // layer instead: copy the region to be kept out to staging first
    // (source and staging never overlap, regardless of scan order),
    // then copy it back from staging to its new, shifted position
    // (staging and the final destination never overlap either). Costs
    // one extra SDRAM-to-SDRAM copy over a single, hardware-handled
    // in-place shift would need, but that's still entirely internal to
    // the chip -- no SPI pixel streaming either way -- and correctness
    // doesn't depend on an internal scan-order guarantee this project
    // has no way to confirm from the documentation available.
    void bteScrollShift(std::int16_t x, std::int16_t y,
                        std::uint16_t w, std::uint16_t h,
                        std::uint16_t shiftRows);
    // Same idea and same reasoning as bteScrollShift() (see its own
    // comment for why this goes via a staging copy rather than a single,
    // potentially-overlapping in-place move), but shifts horizontally --
    // used by the button strip's own slide animation to move its
    // already-drawn pixels instead of re-streaming them from the MCU
    // every step.
    //
    // Unlike bteScrollShift(), w here is the EXACT width of the region
    // to move -- the whole thing shifts, nothing is dropped
    // (bteScrollShift()'s own h includes a row that's meant to fall off
    // the edge; there's no equivalent concept here since this function's
    // own callers always already know precisely which pixels should
    // move and pass exactly that width). dx can be negative (shift
    // left) or positive (shift right).
    void bteHorizontalShift(std::int16_t x, std::int16_t y,
                            std::uint16_t w, std::uint16_t h,
                            std::int16_t dx);

    // ---- PIP menu overlay ----
    // Together, these let UiDialog draw and show the menu as a
    // Picture-in-Picture overlay on a dedicated SDRAM layer
    // (kMenuLayerAddr), entirely separate from the main window (address
    // 0) that the live text screen keeps drawing to underneath -- so
    // opening a menu no longer needs to pause the text screen at all
    // (no more Screen::suspend()/resume()): whatever's on the main
    // window keeps updating live the whole time, just visually covered
    // wherever the PIP window overlaps it.
    //
    // Redirects the drawing engine's own target (Canvas Start Address,
    // CVSSA -- see its own comment in the register list above for why
    // this is a SEPARATE register from Main Image Start Address/MISA,
    // and so doesn't affect what's actually scanned out to the panel at
    // all) to kMenuLayerAddr. Call this before any UiDialog drawing call
    // (drawBox(), drawRow(), txtWrite(), fillRect(), etc.) that's meant
    // to render the menu -- those calls are otherwise unchanged, they
    // just end up writing to the menu's own layer instead of the live
    // main window. Always pair with a matching endOverlayDraw() call
    // once done, even if returning early/on an exception path, since
    // every OTHER drawing call anywhere in this codebase (Screen's own
    // scrolling/new-text included) assumes the canvas is address 0.
    void beginOverlayDraw();
    // Restores CVSSA/CVS_IMWTH0 back to the main window (address 0,
    // width_) -- see beginOverlayDraw()'s own comment.
    void endOverlayDraw();
    // Generic form of beginOverlayDraw(): redirects the drawing engine's
    // canvas to any full-size (width_ x SCREEN_MAX_Y) SDRAM layer, e.g.
    // kTapeLayerAddr. beginOverlayDraw() is simply
    // beginLayerDraw(kMenuLayerAddr). Pair with endOverlayDraw() exactly
    // like beginOverlayDraw() -- every other drawing call assumes the
    // canvas is address 0.
    // stride: the layer's own width in pixels (0 = width_, i.e. a
    // full-size layer); narrower layers such as the button strip's use
    // their own width instead.
    void beginLayerDraw(std::uint32_t layerAddr, std::uint16_t stride = 0);
    // Copies a w x h region from a full-size off-screen layer (drawn via
    // beginLayerDraw()) to the SAME position on the live panel, entirely
    // inside the chip (BTE memory copy, no SPI pixel data). x/y are
    // screen coordinates; vertOffset_ is applied to both source and
    // destination, since beginLayerDraw() content was drawn with it too.
    // Waits for the BTE to finish before returning, so any drawing call
    // can follow directly.
    void copyLayerToPanel(std::uint32_t layerAddr,
                          std::int16_t x, std::int16_t y,
                          std::uint16_t w, std::uint16_t h);
    // General form: copies a w x h region between any two layers (the
    // live panel is address 0, stride width_), each with its own stride.
    // Same rules as copyLayerToPanel(): source and destination must be
    // different layers, vertOffset_ is added to both Y values, and it
    // waits for the BTE to finish before returning.
    void copyLayerRegion(std::uint32_t srcAddr, std::uint16_t srcStride,
                         std::int16_t srcX, std::int16_t srcY,
                         std::uint32_t dstAddr, std::uint16_t dstStride,
                         std::int16_t dstX, std::int16_t dstY,
                         std::uint16_t w, std::uint16_t h);
    // The canvas the drawing engine currently writes to -- as set by
    // beginLayerDraw()/beginOverlayDraw() (or 0 / width_ after
    // endOverlayDraw()). Lets code that must draw into a layer from
    // inside someone else's layer drawing (e.g. while the menu is being
    // drawn) restore the previous canvas afterwards.
    std::uint32_t canvasAddr() const { return canvasAddr_; }
    std::uint16_t canvasStride() const { return canvasStride_; }
    // Configures and enables PIP-1 to show the menu layer's own
    // (x,y,w,h) region as an on-screen overlay at that same (x,y,w,h)
    // position -- i.e. the menu is drawn (via beginOverlayDraw()) at
    // the exact screen coordinates it's meant to appear at, and shown
    // from that same region of the layer, so no separate "windowing"
    // math is needed between where content was drawn and where it's
    // displayed. x and w are rounded down/up respectively to the
    // nearest multiple of 4 -- both REG[2Ah] (on-screen X) and REG[2Eh]/
    // REG[32h] (image address alignment/width) require it (see their
    // own comments in the register list above); y and h have no such
    // requirement (1-pixel vertical unit).
    void showPipOverlay(std::int16_t x, std::int16_t y,
                        std::uint16_t w, std::uint16_t h);
    // Disables PIP-1 -- whatever was already showing on the main window
    // underneath (text, a plotter view, a splash screen -- it was never
    // actually hidden, just covered) becomes visible again immediately,
    // with no redraw needed.
    void hidePipOverlay();
    // General PIP window control (pip = 1 or 2). Shows the w x h region
    // at (srcX, srcY) of the layer at layerAddr (with its own stride) on
    // the panel at (dstX, dstY). srcX, dstX, w and stride must be
    // multiples of 4 (PIP requirement -- masked here to be safe). Y values
    // get vertOffset_ added, like every other panel/layer coordinate.
    // PIP-1 is drawn on top of PIP-2. Can be called repeatedly while the
    // window is enabled, e.g. to animate its size/position.
    // showPipOverlay()/hidePipOverlay() above are the menu's own PIP-1
    // shortcut; the button strip uses PIP-2 (see boardui.cpp).
    void showPipWindow(int pip, std::uint32_t layerAddr, std::uint16_t stride,
                       std::int16_t srcX, std::int16_t srcY,
                       std::int16_t dstX, std::int16_t dstY,
                       std::uint16_t w, std::uint16_t h);
    void hidePipWindow(int pip);

    // Waits for REG[90h] bit4 (BTE Function Enable/Status) to read back
    // 0 ("BTE function is idle") -- per the datasheet's own note that
    // normal host read/write through the canvas/active window isn't
    // allowed while this bit is still 1, this must be called (and
    // return) before any other drawing call that follows a BTE
    // operation.
    // Waits for REG[90h] bit4 (BTE Function Enable/Status) to read back
    // 0 ("BTE function is idle") -- per the datasheet's own note that
    // normal host read/write through the canvas/active window isn't
    // allowed while this bit is still 1, this must be called (and
    // return) before any other drawing call that follows a BTE
    // operation. Returns how many 1ms polling iterations it actually
    // took (0..499), or -1 if it timed out without the bit ever
    // clearing -- see lastBteIdleWaitIters() for why this is tracked.
    int waitBteIdle();
    // How many iterations (or -1 for timeout) the most recent
    // waitBteIdle() call took -- exposed for diagnosing BTE performance
    // (bteMcuWriteBitmap() was confirmed several times slower than the
    // plain MRWDP path it was meant to speed up; this pinpoints whether
    // that's really this wait specifically, and how close to timing out
    // it's actually getting, rather than continuing to guess).
    int lastBteIdleWaitIters() const { return lastBteIdleWaitIters_; }
    // One full 16bpp framebuffer's worth of SDRAM (1024*600*2 bytes)
    // away from address 0 (this project's own live display) -- the
    // manufacturer's own examples always space S0/S1/Destination this
    // far apart; see bteMcuWriteBitmap()'s own comment for why S1 needs
    // a real, distinct address here even though its contents are never
    // actually used.
    static constexpr std::uint32_t kBteLayer2Addr = 1024UL * 600UL * 2UL;
    // One more full layer away from kBteLayer2Addr -- used as a staging
    // area by bteScrollShift() below, kept entirely separate from
    // kBteLayer2Addr's own role (S1's "valid but never read" address)
    // so the two never risk colliding.
    static constexpr std::uint32_t kBteLayer3Addr = 1024UL * 600UL * 2UL * 2UL;
    // A fourth, dedicated layer for the menu's own rendered content --
    // see beginOverlayDraw()'s own comment. Kept entirely separate from
    // the BTE staging layers above (kBteLayer2Addr/kBteLayer3Addr) so a
    // BTE operation mid-flight (e.g. the button strip's own slide
    // animation, or a scroll shift) can never collide with the menu's
    // own, independently-rendered content, and vice versa.
    static constexpr std::uint32_t kMenuLayerAddr = 1024UL * 600UL * 2UL * 3UL;
    // Fifth full-size layer: the pre-rendered HP82161A image shown by the
    // Tape view (see plotterview.cpp). Loaded once at boot from hp82161a.bmp
    // and only ever copied to the panel via copyLayerToPanel() -- never
    // drawn to at runtime, so it can't collide with BTE staging or the
    // menu layer. Lies below kCgramAddr, which is confirmed working on
    // real hardware, so the SDRAM is known to extend past it.
    static constexpr std::uint32_t kTapeLayerAddr = 1024UL * 600UL * 2UL * 4UL;
    // CGRAM (hp82163_font.hpp glyph data uploaded by uploadCgramChar())
    // -- tiny (101 chars * 16 bytes = 1616 bytes total, see
    // FONT_CHAR_COUNT/EXTRA_FONT_COUNT). Its own dedicated layer
    // (layer6 in this board's own confirmed-working layer map, i.e. the
    // 6th 1024*600*2-byte slot, ~7MB in) -- separate from every other
    // layer above so it can never collide with BTE staging or menu
    // rendering, now that the user has confirmed on real hardware that
    // this board's physical SDRAM comfortably extends this far (an
    // EARLIER revision of this constant crammed CGRAM into unused space
    // inside kMenuLayerAddr's own layer specifically because the
    // SDRAM's actual size was still unconfirmed at the time; that
    // workaround is no longer needed).
    static constexpr std::uint32_t kCgramAddr = 1024UL * 600UL * 2UL * 5UL;
    // Button strip layer for PIP-2 (see boardui.cpp) -- narrow (stride =
    // strip width, ~120 px x 600 rows = ~144 kB), so it shares CGRAM's
    // layer slot, 64 kB after CGRAM's 1616 bytes: no overlap, and inside
    // the SDRAM region confirmed working on real hardware.
    static constexpr std::uint32_t kStripLayerAddr = kCgramAddr + 0x10000UL;
    // Tape view cassette (see plotterview.cpp): two full-stride (width_)
    // areas of up to 128 rows each (~256 kB), also in CGRAM's slot after
    // the strip. kCassetteAddr holds the cassette picture as loaded from
    // tape-in.bmp; kCassetteLabelAddr is a copy of it with the current
    // file name written on the label -- that's the one shown.
    static constexpr std::uint32_t kCassetteAddr      = kCgramAddr + 0x40000UL;
    static constexpr std::uint32_t kCassetteLabelAddr = kCgramAddr + 0x80000UL;
    static constexpr std::uint16_t kCassetteMaxRows   = 128;
    // Tape view with the lid OPEN (open.bmp, ~344 x 304): narrow
    // stride (its own width), after the cassette areas in the same slot.
    static constexpr std::uint32_t kTapeOpenAddr      = kCgramAddr + 0xC0000UL;
    static constexpr std::uint32_t kTapeOpenMaxBytes  = 0x60000UL;   // 384 kB
    // Tape view status sprites (leds.bmp, 160 x 40: POWER and BUSY lit,
    // power switch ON), narrow stride, right after the open-lid area
    // (~13 kB).
    static constexpr std::uint32_t kLedsAddr          = kCgramAddr + 0x120000UL;

    // TEMPORARY DIAGNOSTIC -- see lastCvssaReadback()'s own comment above.
    std::uint32_t lastCvssaReadback_ = 0;

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
    // Shows/hides the hardware blinking text cursor at the position last
    // set via txtSetCursor(). Confirmed against the datasheet's own
    // "Graphic / Text Cursor Control Register (GTCCR)" at REG[3Ch] --
    // bit1=Text Cursor Enable, bit0=Blinking Enable. This is a GENUINELY
    // SEPARATE feature from the "Graphic Cursor" (a sprite-like overlay,
    // controlled by REG[40h]/GCHP0 among others) -- Screen.cpp used to
    // poke REG[0x40]/REG[0x4F] directly on every character printed,
    // copying RA8875's own MWCR0/cursor-height register addresses
    // verbatim. On LT7683 those same addresses mean something else
    // entirely (REG[40h]=GCHP0, REG[2Ah]/REG[2Ch]=PIP window position,
    // not cursor position at all) -- writing there on every character was
    // silently corrupting PIP-window/graphic-cursor state instead of
    // showing a text cursor, which is why nothing displayed correctly
    // through the Screen class despite the display driver itself working
    // fine in isolation. blockStyle (underscore vs full-height) isn't
    // wired up on this chip yet -- deferred, since getting text to show
    // at all matters more than the cursor's exact shape.
    //
    // UPDATE: cursor SIZE and BLINK RATE turned out to matter too --
    // both were left at power-on defaults that produced a static-looking
    // "_" on real hardware instead of a blinking block (matching the
    // other panel's own look): CURVS (REG[3Fh], vertical size) defaults
    // to 1 pixel, and BTCR (REG[3Dh], blink time) defaults to toggling
    // every single frame, far too fast to perceive as blinking at all.
    // Both fixed in the .cpp -- see its own comment.
    void setTextCursorVisible(bool visible, bool blockStyle);
    // Prepares for writing many consecutive characters via txtWriteChar()
    // in a tight loop (Screen's own row/scroll redraws). On LT7683 this
    // needs nothing beyond txtMode() -- txtWriteChar() writes through
    // MRWDP (REG[04h]), which auto-increments the cursor on its own in
    // text mode by design, unlike RA8875's REG[40h]/MWCR0 which needs an
    // explicit bit set for the same behaviour. Screen.cpp used to call
    // d_->writeReg(0x40, 0x80) directly here ("text mode, auto-
    // incrementing, invisible cursor" per its own comment) -- correct for
    // RA8875, but REG[40h] is GCHP0 (Graphic Cursor Horizontal Position)
    // on LT7683, not a mode register at all. Harmless in practice (the
    // Graphic Cursor feature is never independently enabled elsewhere, so
    // writing its position does nothing visible), since txtWriteChar()
    // was already self-sufficient regardless -- but misleading to read,
    // and worth a real, correct equivalent rather than a silent no-op.
    void beginBulkTextDraw();
    void txtColor(std::uint16_t fg, std::uint16_t bg);
    void txtTrans(std::uint16_t color);
    void txtSize (std::uint8_t scale);
    void txtWrite(const char* s);
    void txtWriteChar(std::uint8_t c);
    // EXPERIMENTAL/DIAGNOSTIC: writes a character as a 16-bit code
    // (low byte, then high byte) instead of txtWriteChar()'s single
    // byte. Exists to test a specific open question about UCG (see
    // runFontTest() in display_boot_test.hpp for the full context):
    // the datasheet's own CCR0 bit[5:4] note says user-defined
    // character WIDTH depends on whether the character code is above
    // or below 0x8000, which only makes sense if UCG codes are a
    // genuine 15/16-bit value -- but nothing confirms whether the MCU
    // is expected to WRITE that full width per character, or whether a
    // single byte (as CGROM already uses, and as uploadCgramChar()'s
    // own address formula assumes) is auto-extended internally. Not
    // used anywhere in normal operation -- purely for runFontTest() to
    // compare against txtWriteChar()'s own output on real hardware.
    void txtWriteChar16(std::uint16_t code);

    // DIAGNOSTIC: raw single-byte SDRAM access at an ARBITRARY address
    // -- not tied to a CGRAM character code the way uploadCgramChar()/
    // verifyCgramChar() are, just the same underlying CVSSA/linear-mode/
    // MRWDP mechanism generalized. Exists specifically for
    // detectSdramSizeBytes() (display_boot_test.hpp) to empirically
    // measure how much physical SDRAM is actually populated on this
    // board -- see kCgramAddr's own comment (this file) for why that
    // matters: register writes/reads (CVSSA, CGRAM_STR0, CCR0) always
    // succeed regardless of address, since they're just controller-
    // internal storage, but actually reaching SDRAM at an address
    // beyond the physically populated chip silently does nothing.
    void writeSdramByte(std::uint32_t addr, std::uint8_t value);
    std::uint8_t readSdramByte(std::uint32_t addr);

    // Correct multi-byte read from MRWDP (REG[04h]), per the
    // datasheet's own written description of that register's Read
    // Function (confirmed by the user directly against the datasheet
    // text, not just a flowchart image): "if you set this port address
    // from different port address, must issue a dummy read, the first
    // data read cycle is dummy read and data should be ignored."
    // Selects MRWDP ONCE (not per byte -- re-selecting per byte, as an
    // earlier version of this codebase's generic readData(cmd,buf,len)
    // did when pointed at MRWDP, means EVERY byte counts as "setting
    // this port address from a different port address" and gets a
    // dummy response, never real data), issues exactly one dummy read
    // and discards it, then reads `len` real bytes with no further
    // port-address changes in between.
    void readMrwdpBytes(std::uint8_t* buf, std::size_t len);

    // Per the datasheet's own note under REG[5Fh-60h] (CURH, Graphic
    // Read/Write X-Coordinate): "Host should program proper active
    // window related parameters before configure this register." None
    // of uploadCgramChar()/verifyCgramChar()/writeSdramByte()/
    // readSdramByte() ever did this before -- they set CURH0 without
    // ever touching the Active Window registers (REG[56h-5Dh]) at all.
    // widenActiveWindowForLinearAccess() saves whatever the Active
    // Window is CURRENTLY cached as (e.g. clipping set by the menu
    // system, mid-render) and widens it to the full screen so the
    // CURH-based linear address write/read that follows is valid
    // regardless of what any other in-progress drawing operation had
    // it set to; restoreActiveWindow() puts the saved state back
    // afterward so nothing else in the codebase is affected. A
    // saved width/height of 0 means Active Window was never explicitly
    // set before (still at its power-on/never-touched state) --
    // restoreActiveWindow() leaves the full-screen window in place
    // rather than attempting to restore a meaningless 0x0 region.
    void widenActiveWindowForLinearAccess(std::uint16_t* savedX0, std::uint16_t* savedY0,
                                           std::uint16_t* savedW, std::uint16_t* savedH);
    void restoreActiveWindow(std::uint16_t savedX0, std::uint16_t savedY0,
                              std::uint16_t savedW, std::uint16_t savedH);

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
    void roundRect(std::int16_t x, std::int16_t y, std::int16_t w, std::int16_t h,
                    std::uint16_t r, std::uint16_t color);
    void circle    (std::int16_t x, std::int16_t y, std::uint16_t r, std::uint16_t color);
    void fillCircle(std::int16_t x, std::int16_t y, std::uint16_t r, std::uint16_t color);
    void ellipse    (std::int16_t x, std::int16_t y, std::uint16_t rx, std::uint16_t ry, std::uint16_t color);
    void fillEllipse(std::int16_t x, std::int16_t y, std::uint16_t rx, std::uint16_t ry, std::uint16_t color);
    void triangle    (std::int16_t x0, std::int16_t y0, std::int16_t x1, std::int16_t y1,
                       std::int16_t x2, std::int16_t y2, std::uint16_t color);
    void fillTriangle(std::int16_t x0, std::int16_t y0, std::int16_t x1, std::int16_t y1,
                       std::int16_t x2, std::int16_t y2, std::uint16_t color);
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
    // CGRAM custom characters -- see the datasheet's own "Initialize
    // CGRAM from MCU" flowchart (section 8.2.7) and register table
    // (section 14.10, CCR0/CCR1) for the protocol this follows. Confirmed
    // via verifyCgramChar()'s own read-back that the upload itself
    // reaches SDRAM correctly (see pico_main.cpp's own boot-time check);
    // whether CCR0 bit[7:6]=10b (selectCustomFont()) then actually shows
    // it on real hardware is still under investigation -- see Screen.cpp's
    // own comment on its currently-reverted selectCustomFont() call.
    // -----------------------------------------------------------------------
    void uploadCgramChar(std::uint8_t ascii, const std::uint8_t bitmap[16]);
    void uploadCgramFont(std::uint8_t start, std::uint8_t chars, const std::uint8_t *bitmap);

    // Diagnostic only -- reads back the 16 bytes uploadCgramChar() wrote
    // for `ascii`, using the exact same Canvas/CVSSA/MRWDP mechanism
    // symmetrically (read instead of write), and compares against
    // `expected`. Lets pico_main.cpp confirm on real hardware whether
    // the upload itself actually lands in SDRAM the way intended,
    // entirely independent of whether the TEXT-DISPLAY side (CCR0
    // bit[7:6]=10b) then finds/shows it correctly -- narrows down where
    // a "custom font shows nothing" problem actually sits. Deliberately
    // has no LOGF/logging of its own -- this class is part of
    // hipi_core_7/hipi_core_5 (see CMakeLists.txt), shared with the
    // Linux demo build, which never links tinyusb_device and so can't
    // see usb_serial.h's own tusb.h include at all; the actual LOGF
    // calls reporting this function's result live in pico_main.cpp's
    // own initDisplay() instead, which IS part of the USB-linked
    // executable target.
    bool verifyCgramChar(std::uint8_t ascii, const std::uint8_t expected[16], uint8_t *got);

    // TEMPORARY DIAGNOSTIC: the CVSSA0-3 value verifyCgramChar() read
    // straight back (via readData(CVSSA0, ..., 4), the plain
    // one-register-at-a-time path already proven working) right after
    // setting it, from its own most recent call -- lets a caller (e.g.
    // display_boot_test.hpp) confirm whether the ADDRESS itself landed
    // correctly, independent of whether the MRWDP read-back mechanism
    // works. See verifyCgramChar()'s own comment.
    std::uint32_t lastCvssaReadback() const { return lastCvssaReadback_; }

    // TEMPORARY DIAGNOSTIC: reads CGRAM_STR0 (REG[DBh-DEh]) straight
    // back via readData(cmd, buf, len) -- the same one-register-at-a-
    // time path already proven working for CVSSA (see
    // lastCvssaReadback()'s own comment). Confirms whether the ONE
    // writeReg32(CGRAM_STR0, kCgramAddr) call in begin() actually
    // landed, independent of whether CVSSA/the upload itself did --
    // these are two entirely separate registers, so one working
    // doesn't guarantee the other does.
    std::uint32_t readCgramStr0() {
        std::uint32_t v = 0;
        readData(CGRAM_STR0, reinterpret_cast<std::uint8_t*>(&v), 4);
        return v;
    }

    // TEMPORARY DIAGNOSTIC: confirms selectCustomFont()/selectBuiltinFont()'s
    // own writeReg(CCR0_TEXT, ...) actually landed, via the plain,
    // already-proven-working single-byte readReg() (no need for the
    // multi-byte readData() path -- CCR0 is a single register, unlike
    // CVSSA/CGRAM_STR0's 4-byte spread).
    void readCcr0(std::uint8_t* out) { *out = readReg(CCR0_TEXT); }

    // Switches which font source subsequent txtWrite()/txtWriteChar()
    // calls draw from -- see their own .cpp comments for the exact
    // CCR0 (REG[CCh]) bits this sets, confirmed against the datasheet's
    // own register table (section 14.10) rather than guessed.
    // selectCustomFont() shows the hp82163_font.hpp glyphs uploaded by
    // uploadCgramChar() (used for Screen's own HP-41-emulated text);
    // selectBuiltinFont() shows the chip's own internal ISO/IEC 8859-1
    // CGROM instead (used for this project's menus -- see uidialog.hpp's
    // own MenuFrame::draw()). Cheap (one register write), safe to call
    // before every single character drawn -- see Screen.cpp's own
    // comment on why that's necessary rather than relying on whichever
    // mode was last left active.
    void selectCustomFont();

    // Extra spacing ADDED between characters/lines, in pixels, on top of
    // the font's own natural cell width/height (REG[D1h] F2FSSR /
    // REG[D0h] FLDR). Confirmed necessary since the uploaded UCG font
    // uses its full 8-pixel cell width with no built-in gap, so adjacent
    // characters touch with zero spacing otherwise. NOTE: Screen.cpp
    // used to write raw REG[2Eh]/REG[29h] for this directly -- WRONG
    // registers on LT7683 (PIP Image Start Address high byte / Main
    // Window Y position high bits respectively, not spacing at all;
    // those two happen to be the correct registers on RA8875, which is
    // presumably how the mistake happened -- shared Screen.cpp code
    // written for one chip, not updated for the other).
    void setCharSpacing(std::uint8_t pixels);
    void setLineSpacing(std::uint8_t pixels);
    void selectBuiltinFont();

    // -----------------------------------------------------------------------
    // Accessors
    // -----------------------------------------------------------------------
    std::uint16_t width()      const { return width_; }
    std::uint16_t height()     const { return height_; }
    std::uint16_t vertOffset() const { return vertOffset_; }
    std::uint8_t  txtScale()   const { return txtScale_; }

private:
    // See lastBteIdleWaitIters()'s own comment.
    int lastBteIdleWaitIters_ = 0;
    void rectHelper (std::int16_t x1, std::int16_t y1, std::int16_t x2, std::int16_t y2,
                     std::uint16_t color, bool filled);
    void circleHelper(std::int16_t x, std::int16_t y, std::uint16_t r, std::uint16_t color, bool filled);
    void ellipseHelper(std::int16_t x, std::int16_t y, std::uint16_t ha, std::uint16_t va,
                       std::uint16_t color, bool filled);
    void roundRectHelper(std::int16_t x1, std::int16_t y1, std::int16_t x2, std::int16_t y2,
                         std::uint16_t rr, std::uint16_t color, bool filled);
    void triangleHelper(std::int16_t x0, std::int16_t y0, std::int16_t x1, std::int16_t y1,
                        std::int16_t x2, std::int16_t y2, std::uint16_t color, bool filled);

    RA8875Transport& t_;
    std::uint16_t width_;
    std::uint16_t height_;
    std::uint16_t vertOffset_ = 0;
    // Current canvas -- see canvasAddr()/canvasStride()
    std::uint32_t canvasAddr_ = 0;
    std::uint16_t canvasStride_ = 1024;
    std::uint8_t  txtScale_ = 0;
    bool          pwmInitialized_ = false;
    // Software-tracked cache of ICR bit2 (graphic/text mode) -- see
    // gfxMode()/txtMode()'s own comment for why this replaces reading
    // ICR back on every single call.
    enum class GfxTxtMode : std::uint8_t { Unknown, Graphic, Text };
    GfxTxtMode currentGfxTxtMode_ = GfxTxtMode::Unknown;
    // Tracks whether selectCustomFont() (true) or selectBuiltinFont()
    // (false, the power-on default) was called last. Confirmed on real
    // hardware (see txtWriteChar()'s own comment): UCG (custom) text
    // codes MUST be written as two bytes (high byte, then low byte),
    // even for codes that fit in one byte -- CGROM codes are one byte.
    // This lets txtWriteChar() automatically do the right thing for
    // whichever font is currently selected, without every call site
    // throughout the codebase needing to know or care.
    bool usingCustomFont_ = false;
    // Software-tracked cache of CCR1_TEXT bit6 (text background opacity)
    // -- see txtColor()/txtTrans()'s own comments. -1 = unknown (forces
    // the first call to actually read-modify-write and establish a known
    // baseline), 0 = transparent, 1 = opaque.
    std::int8_t opaqueTextCached_ = -1;
    // Software-tracked cache of setTextCursorVisible()'s last-applied
    // parameters -- see its own comment. -1 = unknown (forces the first
    // call to actually write and establish a known baseline).
    std::int8_t cursorVisibleCached_ = -1;
    std::int8_t cursorBlockStyleCached_ = -1;

public:
private:
    // Cache of the last values written via setActiveWindow() -- avoids
    // reading them back from the chip every single scroll (used by
    // clearActiveWindow()'s own fillRect()).
    std::uint16_t activeWindowX0Cached_ = 0;
    std::uint16_t activeWindowY0Cached_ = 0;
    std::uint16_t activeWindowWCached_ = 0;
    std::uint16_t activeWindowHCached_ = 0;
};

}  // namespace hipi
