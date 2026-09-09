// SPDX-License-Identifier: MIT
//
// LT7683 LCD driver -- see LT7683.hpp for the overview of what's grounded
// against the datasheet vs. estimated/pending real-hardware verification.

#include "LT7683.hpp"

#include <algorithm>
#include <cstring>

namespace hipi {

// -----------------------------------------------------------------------
// Construction & init
// -----------------------------------------------------------------------

LT7683::LT7683(RA8875Transport& t,
               std::uint16_t width,
               std::uint16_t height,
               bool start_on)
    : t_(t),
      width_(width),
      height_(height),
      vertOffset_(0),
      txtScale_(0) {
    (void)start_on;  // forwarded to begin()
}

void LT7683::begin() {
    begin(font, FONT_CHAR_COUNT);
}

void LT7683::begin(const std::uint8_t (*font)[FONT_BYTES_PER_CHAR],
                   std::size_t fontChars) {
    (void)font;
    (void)fontChars;  // CGRAM upload deferred -- see uploadCgramChar()'s note

    reset();

    // Confirm the chip left reset in normal operating state (STSR bit1=0)
    // before touching anything else -- matches the datasheet's own
    // recommended check (section 1.2.2, "External Reset").
    for (int i = 0; i < 50; ++i) {
        if ((readStatus() & STSR_INHIBIT) == 0) break;
        t_.delayMs(1);
    }

    // ROOT CAUSE FOUND: pllInit() -- skipped entirely. Confirmed on real
    // hardware: commenting out this single call made the long-hunted
    // "cursor moves, nothing shows" glitch disappear completely, no
    // other change needed. Leaving the chip on its own power-on-default
    // clock configuration instead of reprogramming the PLL. Every other
    // change made during that investigation (caching, register-path
    // switches, SDRAM refresh values, active-window toggling, etc.) was
    // chasing a symptom of this, not a cause -- reverted back out below
    // to keep this simple and fast again.
    //pllInit();

    // ---- SDRAM (Display RAM) init -- section 8 ----
    writeReg(SDRAR, 0x29);
    writeReg(SDRMD, 0x03);
    writeReg(SDR_REF0, 0xE6);
    writeReg(static_cast<std::uint8_t>(SDR_REF0 + 1), 0x01);
    writeReg(SDRCR, 0x01);          // SDR_INITDONE: start SDRAM init
    for (int i = 0; i < 50; ++i) {
        if ((readStatus() & STSR_RAM_READY) != 0) break;
        t_.delayMs(1);
    }

    // ---- Chip/LCD configuration -- confirmed against EastRising's own
    // ESP32 reference firmware for this exact ER-TFTM070-6 module (which
    // targets the same LT768x register map -- same REG[05h]-[0Ah] PLL
    // writes, same REG[E0h]-[E4h] SDRAM sequence -- confirming it's this
    // same chip family, not a different RA8876-only board). ----
    writeReg(CCR, 0x10);    // REG[01h] bit4=1,bit3=0: 16-bit LCD data bus.
                             // bit0 (host bus width) left at 0 -- that bit
                             // only applies to parallel host mode, not our
                             // SPI transport.
    // ---- Display orientation -- CLOSED, confirmed by the manufacturer.
    // This board's panel shows a TRUE 180° rotation with the chip's
    // power-on defaults (confirmed by physically rotating the board:
    // everything then lines up correctly). VDIR (REG[12h] bit3) = 1
    // correctly undoes the vertical half (confirmed: text glyphs render
    // right-side-up, not upside-down -- genuinely re-oriented by the
    // hardware, not just repositioned). MACR (REG[02h]) bit[2:1], which
    // the datasheet's Section 5.4 documents as the horizontal-flip
    // complement needed alongside VDIR for a full 180°, never produced
    // any visible change to actual UI content in ANY combination tried
    // (confirmed not specific to this project's own code -- a second,
    // independent LT7683 driver, ToSStudio's library ported to the Pico
    // SDK, showed the same "no effect" result on the same hardware).
    // Root cause since confirmed directly by the manufacturer: MACR
    // bit[2:1] only affects operations where the HOST itself streams
    // pixel bytes into display RAM via MRWDP -- i.e. loading a bitmap
    // image, matching what this project separately worked out for
    // itself (see drawBitmap565Cropped()'s own comments). It does NOT
    // affect the Geometry Engine or text engine's own internally-
    // generated output, which is almost everything this project actually
    // draws (buttons, panels, text) -- so it was never going to rotate
    // the visible UI, regardless of which combination was tried.
    //
    // The manufacturer also confirmed there is NO register or mechanism
    // on the LT7683 that rotates/mirrors the final panel scan-out itself
    // (the one thing that WOULD affect everything uniformly, the way
    // VDIR does for the vertical axis alone) -- so a real, complete 180°
    // rotation genuinely isn't achievable via registers on this chip.
    // Reverted to the chip's original, unrotated defaults for that
    // reason -- the panel needs to be physically mounted/viewed
    // upside-down.
    //
    // One loose end from this investigation, for whoever revisits VDIR=1
    // in the future: this project separately observed the display stop
    // rendering entirely partway through a boot test with VDIR=1 active,
    // which the manufacturer says shouldn't happen from VDIR=1 alone.
    // Given several OTHER active-window-related rendering bugs were
    // found and fixed elsewhere in this same session (button-strip
    // redraws, status LEDs) that produced superficially similar
    // "stops rendering"/corruption symptoms from an unrelated cause each
    // time, the VDIR=1 regression was most likely one of those same
    // active-window bugs coinciding with VDIR testing, not something
    // VDIR itself caused -- never conclusively re-isolated, though.
    //
    // One confirmed, unrelated correctness fix along the way: bit[7:6] of
    // MACR is "Host Read/Write Image Data Format" (0.0b = Direct Write,
    // correct for this project's SPI interface) -- NOT a colour-depth
    // selector as an earlier pass here assumed, which is why it was
    // wrongly set to 1 (0x40/0x42). Confirmed against the datasheet's own
    // register table, and against a fresh, independent init sequence
    // (the same ported library above) using 0x00 for this register's
    // baseline. Doesn't fix the rotation mystery, but was a real bug
    // regardless -- corrected below.
    writeReg(MACR, 0x00);   // REG[02h]: bit[7:6]=00b (Direct Write, correct
                             // for SPI); bit[2:1]=00b (no rotation/flip).
    writeReg(0x12, 0x80);   // REG[12h] bit7=1: PCLK falling edge; bit3=0:
                             // VDIR normal (top-to-bottom, original
                             // default).
    writeReg(0x13, 0x00);   // REG[13h]: HSYNC low-active (bit7=0), VSYNC
                             // low-active (bit6=0), DE high-active (bit5=0)
                             // -- matches the reference's Active_Polarity
                             // defines exactly.

    // REG[03h] (ICR) bit[1:0]: memory read/write port target -- confirmed
    // missing against EastRising's own Memory_Select_SDRAM(), called right
    // after Graphic_Mode() in their initial(). gfxMode() below only ever
    // touches bit2 (text/graphic mode select) via read-modify-write,
    // leaving bit[1:0] at whatever the power-on default happens to be --
    // if that default doesn't already select SDRAM, every draw/pixel
    // write goes to the wrong physical memory entirely regardless of how
    // correct the Canvas/Main Image addresses are. Explicit, one-time
    // full clear here (not folded into gfxMode() itself, since that's
    // called repeatedly for routine mode switching and this only needs
    // setting once).
    writeData(static_cast<std::uint8_t>(readReg(ICR) & ~0x03));

    // ---- LCD timing -- REG[14h]-[1Fh] ----
    // Register locations/formulas were already confirmed against the
    // datasheet directly (Step 2's first pass). These VALUES are now also
    // confirmed -- computed from EastRising's own reference #defines for
    // this exact panel (LCD_HBPD=144, LCD_HFPD=160, LCD_HSPW=20,
    // LCD_VBPD=20, LCD_VFPD=12, LCD_VSPW=3), run through the exact same
    // integer-truncating division their own firmware uses (confirmed by
    // reading their LCD_Horizontal_Non_Display() etc. implementations
    // directly, not just the datasheet's abstract formula):
    //   HNDR   = (144/8)-1 = 17           HPWR = (20/8)-1 = 1 (truncated,
    //   HSTR   = (160/8)-1 = 19                  not rounded -- 16px
    //   VNDR   = 20-1 = 19                       actual, not 20)
    //   VSTR   = 12-1 = 11
    //   VPWR   = 3-1  = 2
    const std::uint16_t hdwr = static_cast<std::uint16_t>(width_ / 8 - 1);
    writeReg(HDWR, static_cast<std::uint8_t>(hdwr));
    writeReg(HDWFTR, static_cast<std::uint8_t>(width_ % 8));
    writeReg(HNDR, 17);
    writeReg(HNDFTR, 0);
    writeReg(HSTR, 19);
    writeReg(HPWR, 1);
    writeReg16(VDHR0, static_cast<std::uint16_t>(height_ - 1));
    writeReg16(VNDR0, 19);
    writeReg(VSTR, 11);
    writeReg(VPWR, 2);

    // Main window: single full-panel canvas, 16bpp, starting at address 0.
    //
    // IMPORTANT: Main Image (REG[20h-25h], what's actually scanned out to
    // the panel) and Canvas Image (REG[50h-55h], what the drawing engine
    // itself targets) are SEPARATE address registers on this chip -- an
    // earlier pass here only set Main Image, leaving Canvas at its
    // power-on-default address/width. Every draw call succeeded (correct
    // status, no errors) but silently wrote into a region of SDRAM the
    // panel was never scanning from, so nothing ever appeared on screen.
    // Confirmed against EastRising's own reference firmware, which always
    // sets both to the same place (see its main loop() calling
    // Main_Image_Start_Address()+Canvas_Image_Start_address() together
    // every frame). Both are kept in lockstep here for the same reason.
    writeReg16(MISA0, 0);
    writeReg16(static_cast<std::uint8_t>(MISA0 + 2), 0);
    writeReg16(MIW0, width_);
    writeReg16(MWSXY0, 0);                                  // Main Window Start X = 0
    writeReg16(static_cast<std::uint8_t>(MWSXY0 + 2), 0);   // Main Window Start Y = 0
    writeReg16(CVSSA0, 0);
    writeReg16(static_cast<std::uint8_t>(CVSSA0 + 2), 0);
    writeReg16(CVS_IMWTH0, width_);
    writeReg(0x10, 0x04);    // REG[10h] bit[3:2]=01b: Main Window 16bpp
    writeReg(AW_COLOR, 0x01);  // REG[5Eh] bit[1:0]: Active Window/canvas
                               // memory access mode = 16bpp block (X-Y)
    setActiveWindow(0, 0, static_cast<std::uint16_t>(width_ - 1),
                    static_cast<std::uint16_t>(height_ - 1));

    gfxMode();
    fill(0x0000);
    turnOn(true);
    brightness(200);
}

// -----------------------------------------------------------------------
// Low-level register access -- 4-wire SPI. Confirmed against Figure 2-6
// (3-Wire SPI Interface Timing): the first byte's bit7=A0, bit6=RW#,
// bits5-0 don't-care; the second byte is the actual address/data. Same
// two-byte-transfer shape as RA8875Transport already expects, just with
// LT7683's own A0/RW# encoding (see CMDWR/DATWR/CMDRD/DATRD in the .hpp).
// -----------------------------------------------------------------------

void LT7683::writeReg(std::uint8_t cmd, std::uint8_t data) {
    writeCmd(cmd);
    writeData(data);
}

void LT7683::writeReg(std::uint8_t cmd, const std::uint8_t* data, std::size_t len) {
    writeCmd(cmd);
    writeData(data, len);
}

void LT7683::writeReg16(std::uint8_t cmd, std::uint16_t data) {
    writeCmd(cmd);
    writeData(static_cast<std::uint8_t>(data & 0xFF));
    writeCmd(static_cast<std::uint8_t>(cmd + 1));
    writeData(static_cast<std::uint8_t>((data >> 8) & 0xFF));
}

void LT7683::writeCmd(std::uint8_t cmd) {
    const std::uint8_t tx[2] = { CMDWR, cmd };
    t_.csLow();
    t_.spiTransfer(tx, nullptr, 2);
    t_.csHigh();
}

void LT7683::writeData(std::uint8_t data) {
    const std::uint8_t tx[2] = { DATWR, data };
    t_.csLow();
    t_.spiTransfer(tx, nullptr, 2);
    t_.csHigh();
}

void LT7683::writeData(const std::uint8_t* data, std::size_t len) {
    if (len == 0) return;
    t_.csLow();
    t_.spiTransfer(&DATWR, nullptr, 1);
    t_.spiTransfer(data, nullptr, len);
    t_.csHigh();
}

std::uint8_t LT7683::readReg(std::uint8_t cmd) {
    writeCmd(cmd);
    return readData();
}

std::uint8_t LT7683::readData() {
    std::uint8_t rx[1] = { 0 };
    t_.csLow();
    t_.spiTransfer(&DATRD, nullptr, 1);
    t_.spiTransfer(nullptr, rx, 1);
    t_.csHigh();
    return rx[0];
}

std::uint8_t LT7683::readStatus() {
    std::uint8_t rx[1] = { 0 };
    t_.csLow();
    t_.spiTransfer(&CMDRD, nullptr, 1);
    t_.spiTransfer(nullptr, rx, 1);
    t_.csHigh();
    return rx[0];
}

void LT7683::waitPoll(std::uint8_t reg, std::uint8_t mask) {
    // Check immediately first, then delay if still busy -- sleeping
    // before the first check just wastes time on nearly every hardware
    // draw call, since the bit is very often already clear.
    for (int i = 0; i < 2500; ++i) {
        if ((readReg(reg) & mask) == 0) return;
        t_.delayUs(20);
    }
    // Reached the end of the loop without the busy bit ever clearing --
    // give up and let the caller carry on rather than hang forever.
}

void LT7683::waitStatus(std::uint8_t mask) {
    for (int i = 0; i < 2500; ++i) {
        if ((readStatus() & mask) == 0) return;
        t_.delayUs(20);
    }
}

// -----------------------------------------------------------------------
// BTE
// -----------------------------------------------------------------------

void LT7683::BTE(std::uint8_t opcode,
                 std::uint16_t x1, std::uint16_t y1,
                 std::uint16_t w, std::uint16_t h,
                 std::uint16_t x0, std::uint16_t y0) {
    // Kept only for interface compatibility -- unused parameters silence
    // warnings. See bteMcuWriteBitmap() below for the actual, working
    // BTE implementation this project now uses.
    (void)opcode; (void)x1; (void)y1; (void)w; (void)h; (void)x0; (void)y0;
}

// -----------------------------------------------------------------------
// BTE MCU Write with ROP -- streams pixel data from MCU memory straight
// into a rectangular region of the visible display, via the BTE engine
// instead of plain MRWDP writes (see drawBitmap565Cropped() for the
// non-BTE equivalent this is meant to speed up).
//
// Root-caused via the manufacturer's own Application Note
// (L768_AP-Note_ENG_V1_2.pdf, sections 10.1/10.2.1, pages 61 and 63-64)
// and a follow-up direct answer from the manufacturer's own engineer,
// after every earlier attempt at BTE on this project froze the panel:
//
// 1. S1 must ALWAYS be configured with a valid start address, even in
//    operation modes (like this one) whose ROP function completely
//    ignores S1's actual contents -- confirmed by BOTH official example
//    flowcharts (Memory Copy AND MCU Write) always including an S1 setup
//    step. This project's own earlier attempt left S1 untouched entirely
//    (following a different, unrelated reference driver's own -- for
//    this chip, incorrect -- assumption that S1 could be skipped).
//
// 2. S0, S1, and Destination must use DISTINCT, non-overlapping SDRAM
//    addresses -- confirmed directly by the manufacturer ("you need to
//    be very careful with each layer's SDRAM address") and by every
//    official example, which always spaces them a full layer apart
//    (1024*600*2 bytes, one whole 16bpp framebuffer's worth) -- e.g.
//    layer1=0 (the live, actively-scanned-out display), layer2=1024*600*2,
//    layer3=1024*600*2*2, etc. This project's own earlier attempt used
//    address 0 for BOTH the source AND destination of a Memory Copy --
//    reading from and writing to the SAME live framebuffer region BTE
//    was simultaneously scanning out to the panel, which is almost
//    certainly what actually froze the display, independent of the
//    missing-S1 issue above.
//
// This function sidesteps the address-collision risk entirely by using
// BTE's own "MCU Write with ROP" mode (opcode 0000b) instead of "Memory
// Copy" (0010b): the real pixel data streams directly from MCU RAM (this
// project's own buttonStripPixels, exactly like drawBitmap565Cropped()
// already does over plain MRWDP) rather than needing to be pre-loaded
// into a second SDRAM layer first. S1 still needs a valid, distinct
// address (see point 1 above) even though ROP 0xC ("DT = S0") means its
// contents are completely ignored -- kBteLayer2Addr, one full layer away
// from the live display at address 0, same spacing the manufacturer's
// own examples use.
void LT7683::bteMcuWriteBitmap(std::int16_t destX, std::int16_t destY,
                               std::uint16_t drawWidth, std::uint16_t h,
                               std::uint16_t srcStride,
                               const std::uint16_t* data) {
    // Precondition per the datasheet's own REG[90h] bit4 description:
    // "When BTE function enable, Normal Host R/W memory through canvas
    // doesn't allow" -- i.e. a PRIOR BTE operation must be fully
    // finished (not just triggered) before starting a new one, or before
    // this project's own other drawing calls touch the canvas at all.
    waitBteIdle();

    gfxMode();

    // ---- S1: valid, distinct, off-screen address -- see this
    // function's own file-header comment for why this can't just be
    // skipped or left at 0 despite never actually being read for this
    // ROP mode. Width/X/Y are otherwise irrelevant (never used by ROP
    // 0xC), kept minimal but valid (4 is the smallest legal width, since
    // REG[A1h] bit[1:0] must be 0 as documented, i.e. divisible by 4).
    writeReg16(S1_STR0, static_cast<std::uint16_t>(kBteLayer2Addr & 0xFFFF));
    writeReg16(static_cast<std::uint8_t>(S1_STR0 + 2),
              static_cast<std::uint16_t>((kBteLayer2Addr >> 16) & 0xFFFF));
    writeReg16(S1_WTH0, 4);
    writeReg16(S1_X0, 0);
    writeReg16(S1_Y0, 0);

    // ---- Destination: address 0 -- this project's own live display
    // (layer1, per the manufacturer's own addressing scheme), at its
    // real, full panel stride (NOT drawWidth -- DT_WTH0 is address-0's
    // own natural row width, independent of how wide THIS PARTICULAR
    // draw's BTE window is).
    writeReg16(DT_STR0, 0);
    writeReg16(static_cast<std::uint8_t>(DT_STR0 + 2), 0);
    writeReg16(DT_WTH0, width_);
    writeReg16(DT_X0, static_cast<std::uint16_t>(destX));
    writeReg16(DT_Y0, static_cast<std::uint16_t>(destY + vertOffset_));

    // ---- BTE window: how many pixels this specific draw covers.
    writeReg16(BLT_WTH0, drawWidth);
    writeReg16(BLT_HIG0, h);

    // REG[92h] (BLT_COLR): bit[6:5]=01b (S0 16bpp), bit[4:2]=001b (S1
    // 16bpp), bit[1:0]=01b (Destination 16bpp).
    writeReg(BLT_COLR, 0x25);

    // REG[91h]: ROP[7:4]=0xC ("DT = S0", i.e. show only the MCU-streamed
    // data, completely ignoring S1) | operation code[3:0]=0000b (MCU
    // Write with ROP).
    writeReg(BLT_CTRL1, 0xC0);

    // REG[90h] bit4=1: BTE enable/start.
    writeReg(BLT_CTRL0, 0x10);

    // Stream the pixel data in fixed-size chunks -- matches
    // drawBitmap565Cropped()'s own approach exactly (see its own
    // comment for why chunking + a FIFO check per chunk, rather than
    // per pixel, matters). The first (much slower) version of this
    // function checked the FIFO and called writeData() twice PER PIXEL
    // -- each writeData() call is its own individual SPI transaction
    // (CS toggled low/high around just 2 bytes), so that was 3 separate
    // SPI transactions per pixel (one FIFO-status read, two 1-byte
    // writes) versus this chunked version's one FIFO-status read plus
    // one bulk write per 32 pixels -- confirmed on real hardware to be
    // the actual reason BTE measured *slower* than the plain MRWDP path
    // it was meant to speed up, not BTE itself or waitBteIdle() (that
    // measured instantly, 0 iterations, every time).
    //
    // Unlike drawBitmap565Cropped(), this can stream the ENTIRE region
    // as one continuous run of chunks -- no per-row cursor repositioning
    // needed, since BTE's own BLT_WTH0/BLT_HIG0 window (configured
    // above) already tells the chip where each row wraps internally.
    static std::uint8_t chunkBuf[1024 * 2];
    constexpr std::size_t kChunkBytes = 64;
    writeCmd(MRWDP);
    for (std::uint16_t row = 0; row < h; ++row) {
        const std::uint16_t* src = data + static_cast<std::size_t>(row) * srcStride;
        for (std::uint16_t col = 0; col < drawWidth; ++col) {
            chunkBuf[col * 2]     = static_cast<std::uint8_t>(src[col] & 0xFF);
            chunkBuf[col * 2 + 1] = static_cast<std::uint8_t>(src[col] >> 8);
        }
        const std::size_t totalBytes = static_cast<std::size_t>(drawWidth) * 2;
        std::size_t offset = 0;
        while (offset < totalBytes) {
            const std::size_t n = std::min(kChunkBytes, totalBytes - offset);
            for (int i = 0; i < 100; ++i) {
                if ((readStatus() & STSR_WR_FIFO_FULL) == 0) break;
                t_.delayMs(1);
            }
            writeData(chunkBuf + offset, n);
            offset += n;
        }
    }

    // Wait for BTE to actually finish (REG[90h] bit4 back to 0) before
    // returning -- see this function's own comment on why leaving it
    // busy would block any of this project's other drawing calls that
    // follow.
    waitBteIdle();
}

int LT7683::waitBteIdle() {
    for (int i = 0; i < 500; ++i) {
        if ((readReg(BLT_CTRL0) & 0x10) == 0) {
            lastBteIdleWaitIters_ = i;
            return i;
        }
        t_.delayMs(1);
    }
    lastBteIdleWaitIters_ = -1;
    return -1;  // timed out
}

// -----------------------------------------------------------------------
// Power / reset / PLL / backlight
// -----------------------------------------------------------------------

void LT7683::reset() {
    t_.rstLow();
    t_.delayMs(100);
    t_.rstHigh();
    t_.delayMs(100);
}

void LT7683::softReset() {
    writeReg(SRR, 0x01);   // REG[00h] bit0: software reset
    t_.delayMs(1);
}

void LT7683::turnOn(bool on) {
    // Display enable lives in the Display Configuration register --
    // REG[12h] bit6 per this datasheet family's usual convention (Display
    // On/Off). Kept as a single explicit bit set/clear so it doesn't
    // disturb the other bits (colour bar, VDIR, PIP enables) in that
    // register.
    const std::uint8_t cur = readReg(0x12);
    writeData(on ? static_cast<std::uint8_t>(cur | 0x40)
                 : static_cast<std::uint8_t>(cur & ~0x40));
}

void LT7683::sleep(bool s) {
    // REG[E4h] bit1: SDR_PSAVING -- 0->1 enters power saving, 1->0 exits.
    const std::uint8_t cur = readReg(SDRCR);
    writeData(s ? static_cast<std::uint8_t>(cur | 0x02)
                : static_cast<std::uint8_t>(cur & ~0x02));
}

void LT7683::brightness(std::uint8_t level) {
    // PWM0, normal polarity (duty = level) -- confirmed correct on real
    // hardware via runBacklightSweepTest() (display_boot_test.hpp), which
    // swept all 4 channel/polarity combinations; this was the only one
    // that visibly changed the backlight. (An earlier pass here had
    // switched to PWM1 based on RA8876_Lite's own backlight example using
    // that channel -- a reasonable guess from a reference board, but this
    // board's actual wiring turned out to use PWM0 instead.)
    //
    // One-time setup, confirmed against the datasheet's Table 9-1 (REG[85h]
    // Description): bit[1:0] must be 10b to route Timer-0's PWM output to
    // the physical XPWM0 pin (left at its power-on default -- plain GPIO
    // mode -- otherwise, which is why writing only the duty register alone
    // never had any visible effect before this was added).
    //
    // PWM_CLK = CCLK/(Prescaler+1) = 100MHz/100 = 1MHz (REG[84h]=99).
    // Timer-0 clock divisor left at 00b (/1, REG[85h] bit[5:4]=00), so
    // Timer-0 counts at 1MHz too. TCNTB0=255 sets the PWM period to 255
    // clocks (~3.9kHz), matching this function's own 0-255 level range
    // 1:1 with no scaling needed for the duty write below.
    if (!pwmInitialized_) {
        writeReg(PSCLR, 99);      // REG[84h]
        writeReg(PMUXR, 0x02);    // REG[85h]: bit[1:0]=10b (PWM0->Timer0), bit[5:4]=00b (/1)
        writeReg16(0x8A, 255);    // REG[8Ah-8Bh] (TCNTB0): period
        writeReg(PCFGR, 0x03);    // REG[86h]: bit1=1 (auto-reload), bit0=1 (Timer-0 start)
        pwmInitialized_ = true;
    }
    // REG[88h-89h] (TCMPB0): duty cycle. Normal polarity -- confirmed.
    writeReg16(0x88, level);
}

void LT7683::pllInit() {
    // Confirmed against EastRising's own ESP32 reference firmware for this
    // exact ER-TFTM070-6 module -- same register addresses (0x05-0x0A) as
    // derived from the datasheet directly, and these exact byte values
    // (copied verbatim rather than re-derived through this project's own
    // OD/R/N bit-packing, since that independent derivation didn't
    // reproduce their values exactly -- safer to trust the
    // vendor-tested-on-this-panel numbers than to re-guess the encoding):
    //   XI (crystal)      = 10MHz
    //   PCLK  (REG05h/06h) = 0x06, 39   -> ~50MHz (panel scan clock)
    //   MCLK  (REG07h/08h) = 0x04, 39   -> ~100MHz (SDRAM clock)
    //   CCLK  (REG09h/0Ah) = 0x04, 39   -> ~100MHz (core clock)
    // Satisfies the datasheet's own "CCLK*2>=MCLK>=CCLK, CCLK>=PCLK*1.5"
    // rules (section 6.1) comfortably.
    writeReg(PPLLC1, 0x06);
    writeReg(PPLLC2, 39);
    writeReg(MPLLC1, 0x04);
    writeReg(MPLLC2, 39);
    writeReg(CPLLC1, 0x04);
    writeReg(CPLLC2, 39);
    // Reconfigure PLL frequency -- REG[00h] bit7 (per this datasheet's own
    // "Software Reset Register" first bit described in section 14.2,
    // matching EastRising's own PLL_Initial() which writes 0x80 to
    // REG[00h] right after setting the PLL registers).
    writeReg(SRR, 0x80);
    t_.delayMs(1);
}

// -----------------------------------------------------------------------
// Mode
// -----------------------------------------------------------------------

void LT7683::gfxMode() {
    // Writes the full, known-correct ICR byte directly (bits[1:0]=00b
    // always, matching the one-time SDRAM-target clear done in begin();
    // bit2 the only thing that ever changes) instead of a read-modify-
    // write -- avoids ever reading ICR back (a read that, if ever wrong,
    // could permanently redirect MRWDP writes off-screen -- see ICR's
    // own bit[1:0] description). Cached-and-skipped when already in this
    // mode -- cheap, and safe now that the actual root cause (pllInit()
    // misconfiguring the clock -- see begin()'s own comment) is gone.
    if (currentGfxTxtMode_ == GfxTxtMode::Graphic) return;
    writeCmd(ICR);
    writeData(0x00);  // ICR = 0x00: bit2=0 (graphic mode), bits[1:0]=00b (Display RAM)
    currentGfxTxtMode_ = GfxTxtMode::Graphic;
}

void LT7683::txtMode() {
    // See gfxMode()'s own comment.
    if (currentGfxTxtMode_ == GfxTxtMode::Text) return;
    writeCmd(ICR);
    writeData(0x04);  // ICR = 0x04: bit2=1 (text mode), bits[1:0]=00b (Display RAM)
    currentGfxTxtMode_ = GfxTxtMode::Text;
}

// -----------------------------------------------------------------------
// Colour + cursor + text
// -----------------------------------------------------------------------

void LT7683::setColor(std::uint16_t color) {
    writeReg(FGCR, static_cast<std::uint8_t>((color & 0xF800) >> 8));
    writeReg(FGCG, static_cast<std::uint8_t>((color & 0x07E0) >> 3));
    writeReg(FGCB, static_cast<std::uint8_t>((color & 0x001F) << 3));
}

void LT7683::setBgColor(std::uint16_t color) {
    writeReg(BGCR, static_cast<std::uint8_t>((color & 0xF800) >> 8));
    writeReg(BGCG, static_cast<std::uint8_t>((color & 0x07E0) >> 3));
    writeReg(BGCB, static_cast<std::uint8_t>((color & 0x001F) << 3));
}

void LT7683::setxy(std::uint16_t x, std::uint16_t y) {
    gfxMode();
    writeReg16(CURH0, x);
    writeReg16(CURV0, static_cast<std::uint16_t>(y + vertOffset_));
}

void LT7683::txtSetCursor(std::uint16_t x, std::uint16_t y) {
    txtMode();
    writeReg16(F_CURX0, x);
    writeReg16(F_CURY0, static_cast<std::uint16_t>(y + vertOffset_));
}

void LT7683::setTextCursorVisible(bool visible, bool blockStyle) {
    // Screen's own set_cur() (which calls this) fires after every
    // printable character during ordinary typing, not just on scroll/
    // redraw -- cached and skipped when visible/blockStyle are unchanged
    // from the previous call (the overwhelming majority of calls) to
    // avoid 4 redundant register writes (REG[3Ch]-[3Fh]) per character.
    if (cursorVisibleCached_ == visible && cursorBlockStyleCached_ == blockStyle) return;
    cursorVisibleCached_ = visible;
    cursorBlockStyleCached_ = blockStyle;
    // REG[3Ch] (GTCCR) bit1=Text Cursor Enable, bit0=Blinking Enable --
    // see this method's own header comment in the .hpp for why this
    // (rather than RA8875's REG[40h]/REG[4Fh]) is the correct register
    // for LT7683.
    (void)blockStyle;
    if (visible) {
        // REG[3Eh] (CURHS) / REG[3Fh] (CURVS): cursor size in BASE
        // (unscaled) pixels -- confirmed against the datasheet's own note
        // that "When character is enlarged, the cursor setting will
        // multiply the same times as the character enlargement", i.e.
        // the hardware scales this automatically to match whatever
        // txtSize() enlargement is active, so this only needs setting
        // once here to the BASE 8x16 font cell, not per txtScale_.
        // CURHS default (7 -> 8px) already happens to match the base
        // font's 8px width, left as-is; CURVS's own default (0 -> 1px)
        // is what was actually producing the thin "_" look reported on
        // real hardware -- 15 -> 16px matches the base font's height,
        // giving a real full-height block cursor once scaled.
        writeReg(0x3E, 7);
        writeReg(0x3F, 15);
        // REG[3Dh] (BTCR): blink time, in frame-cycles per toggle. Left
        // at its power-on default (0x00, "1 Frame Cycle Time") this
        // toggles every single frame -- at a normal LCD refresh rate
        // that's far too fast to perceive as blinking at all, which
        // reads as a solid, non-blinking cursor even with GTCCR's own
        // blink-enable bit correctly set. ~0.75s per full on/off cycle
        // at a typical ~60Hz panel refresh.
        writeReg(0x3D, 0x2D);
    }
    writeReg(0x3C, visible ? 0x03 : 0x00);
}

void LT7683::beginBulkTextDraw() {
    // See this method's own header comment -- text mode is the only part
    // that needed a real LT7683 equivalent for RA8875's original "auto-
    // incrementing" intent (txtWriteChar()'s own MRWDP write already
    // handles that on its own). But RA8875's single REG[40h]=0x80 write
    // did THREE things at once per its own comment -- "text mode, auto-
    // incrementing, invisible cursor" -- and this was only replicating
    // the first two. Confirmed on real hardware: without also hiding the
    // cursor here, it visibly jumped around the screen during every
    // clear()/full()/scroll redraw (each draw_letter() repositioning it
    // along the way), something RA8875 never showed since its own single
    // write already covered this. setTextCursorVisible() (see its own
    // comment) is what set_cur() -- always called again right after
    // these bulk draws finish -- uses to correctly restore visibility
    // afterward, so this only needs to hide it, not manage restoring it.
    setTextCursorVisible(false, false);
    txtMode();
}

void LT7683::txtColor(std::uint16_t fg, std::uint16_t bg) {
    setColor(fg);
    setBgColor(bg);
    // CCR1_TEXT (REG[CDh]) bit6: background transparency -- 0=opaque
    // (uses BGC*), matches RA8875's txtColor() semantics (opaque, not
    // transparent background). Cached and skipped when already opaque
    // (the overwhelming majority of calls -- txtTrans() is the only
    // thing that ever sets transparent).
    if (opaqueTextCached_ != 1) {
        writeData(static_cast<std::uint8_t>(readReg(CCR1_TEXT) & ~(1 << 6)));
        opaqueTextCached_ = 1;
    }
}

void LT7683::txtTrans(std::uint16_t color) {
    txtMode();
    setColor(color);
    if (opaqueTextCached_ != 0) {
        writeData(static_cast<std::uint8_t>(readReg(CCR1_TEXT) | (1 << 6)));
        opaqueTextCached_ = 0;
    }
}

void LT7683::txtSize(std::uint8_t scale) {
    txtMode();
    if (scale > 3) scale = 3;
    // Confirmed against EastRising's Font_Width_X1..X4()/Font_Height_X1..
    // X4(): horizontal enlargement is REG[CDh] bit[3:2], vertical is
    // bit[1:0] -- NOT REG[CCh] (CCR0_TEXT), which is font *select*
    // (internal CGROM vs user-defined, and font height 8x16/12x24/16x32
    // in its own bit[5:4]) and has nothing to do with enlargement. An
    // earlier pass here wrote this same (scale<<2)|scale pattern to the
    // wrong register entirely -- harmless to CCR0_TEXT's own bits (only
    // ever touched bit[3:0], preserving font-select), but meant the
    // actual enlarge register never moved off its power-on default.
    writeData(static_cast<std::uint8_t>((readReg(CCR1_TEXT) & ~0x0F) | ((scale << 2) | scale)));
    txtScale_ = scale;
}

void LT7683::txtWrite(const char* s) {
    txtMode();
    writeCmd(MRWDP);
    for (const char* p = s; *p; ) {
        const std::uint8_t b0 = static_cast<std::uint8_t>(*p);
        std::uint8_t out;
        if (b0 == 0xC3 && p[1] != '\0') {
            // Same 2-byte UTF-8 -> Latin-1 reconstruction as RA8875's own
            // txtWrite() -- see hp82163_font.hpp's extra_font[].
            const std::uint8_t b1 = static_cast<std::uint8_t>(p[1]);
            out = static_cast<std::uint8_t>(0xC0 | (b1 & 0x3F));
            p += 2;
        } else {
            out = b0;
            ++p;
        }
        writeData(out);
        // See txtWriteChar()'s own comment for why this matters.
        waitStatus(STSR_CORE_BUSY);
    }
}

void LT7683::txtWriteChar(std::uint8_t c) {
    txtMode();
    writeCmd(MRWDP);
    writeData(c);
    // Matches the reference driver's own Check_Busy_Draw() -- an
    // unconditional wait for STSR_CORE_BUSY to clear after every single
    // character write. Not just PLL-timing compensation: removing this
    // entirely (as an earlier cleanup pass mistakenly did, along with
    // genuinely unnecessary settling margins/FIFO polling) caused
    // corrupted glyph rendering at every font size, including the
    // splash dialog's own unscaled text -- the chip needs this to
    // finish rendering one character before the next write arrives.
    waitStatus(STSR_CORE_BUSY);
}

// -----------------------------------------------------------------------
// Primitives -- geometric drawing engine, REG[67h]-[7Eh]
// -----------------------------------------------------------------------

void LT7683::pixel(std::int16_t x, std::int16_t y, std::uint16_t color) {
    setxy(static_cast<std::uint16_t>(x), static_cast<std::uint16_t>(y));
    const std::uint8_t data[2] = {
        static_cast<std::uint8_t>(color >> 8),
        static_cast<std::uint8_t>(color & 0xFF)
    };
    writeReg(MRWDP, data, 2);
}

void LT7683::drawBitmap565(std::int16_t x, std::int16_t y,
                           std::uint16_t w, std::uint16_t h,
                           const std::uint16_t* data) {
    drawBitmap565Cropped(x, y, w, h, w, data);
}

void LT7683::drawBitmap565Cropped(std::int16_t x, std::int16_t y,
                                  std::uint16_t drawWidth, std::uint16_t h,
                                  std::uint16_t srcStride,
                                  const std::uint16_t* data) {
    gfxMode();
    static std::uint8_t rowBuf[1024 * 2];
    if (drawWidth > 1024) return;

    // Chunked, FIFO-aware writes -- confirmed necessary on real hardware:
    // without this, wide bitmaps (button strip, logo) came out with
    // horizontal colour banding. writeData(buf, len)'s bulk overload
    // blasts the whole buffer over SPI in one continuous transfer with no
    // flow control at all, trusting the chip's own Memory Write FIFO to
    // keep up -- reasonable for the short bursts every OTHER caller uses
    // it for (a handful of register bytes), but a full-width row here can
    // be far larger than that FIFO's actual depth. Reference libraries
    // consulted throughout this project's LT7683 bring-up (RA8876_Lite,
    // TeensyRA8876) all poll Status Register bit7 (Write FIFO Full)
    // during exactly this kind of bulk pixel transfer -- this does the
    // same, breaking each row into fixed-size chunks and waiting for the
    // FIFO to drain between them, rather than assuming a single
    // buf/len write is always safe.
    constexpr std::size_t kChunkBytes = 64;

    for (std::uint16_t row = 0; row < h; ++row) {
        // NOT calling setxy() here -- it calls gfxMode() internally on
        // every single call, which itself does a full ICR read-then-
        // write (two separate SPI transactions) every time. For a tall
        // bitmap that meant 480-600 completely redundant ICR round-trips
        // per draw (already confirmed to be in graphics mode once, right
        // above, before this loop even starts) -- confirmed on real
        // hardware to correlate with severe colour corruption (wrong
        // hues, e.g. a yellow frame rendering pink/violet, black
        // rendering light green) alongside the horizontal banding this
        // function's chunking fix was originally meant to solve.
        // Positioning directly via the same two registers setxy() itself
        // uses, minus its gfxMode() call, removes that redundant
        // per-row SPI traffic entirely.
        writeReg16(CURH0, static_cast<std::uint16_t>(x));
        writeReg16(CURV0, static_cast<std::uint16_t>(y + row + vertOffset_));
        writeCmd(MRWDP);

        const std::uint16_t* src = data + static_cast<std::size_t>(row) * srcStride;
        for (std::uint16_t col = 0; col < drawWidth; ++col) {
            // Low byte first, then high -- confirmed backwards before this
            // fix. This is the ONLY code path in the whole project that
            // sends a packed 16-bit RGB565 value as two raw bytes over
            // MRWDP -- every other colour operation (fillRect, text, etc.)
            // goes through setColor(), which writes R/G/B to three
            // SEPARATE 8-bit registers (FGCR/FGCG/FGCB), not a packed
            // 16-bit value at all -- so this specific byte order was never
            // actually exercised/validated anywhere else in this project.
            // Confirmed wrong on real hardware via the colour math: a
            // source yellow (R=255,G=255,B=0 -> RGB565 0xFFE0) sent
            // high-byte-first reconstructs, if the chip actually wants
            // low-byte-first, as 0xE0FF -- which decodes to high red, LOW
            // green, high blue: magenta/pink. That's exactly the reported
            // symptom (a yellow frame rendering pink/violet).
            rowBuf[col * 2]     = static_cast<std::uint8_t>(src[col] & 0xFF);
            rowBuf[col * 2 + 1] = static_cast<std::uint8_t>(src[col] >> 8);
        }

        const std::size_t totalBytes = static_cast<std::size_t>(drawWidth) * 2;
        std::size_t offset = 0;
        while (offset < totalBytes) {
            const std::size_t n = std::min(kChunkBytes, totalBytes - offset);
            for (int i = 0; i < 100; ++i) {
                if ((readStatus() & STSR_WR_FIFO_FULL) == 0) break;
                t_.delayMs(1);
            }
            writeData(rowBuf + offset, n);
            offset += n;
        }
    }
}

void LT7683::drawBitmap332(std::int16_t x, std::int16_t y,
                           std::uint16_t w, std::uint16_t h,
                           const std::uint8_t* data) {
    // Not used anywhere in this project currently (RA8875's own
    // drawBitmap332() is likewise unused in the codebase as of Step 1) --
    // left unimplemented rather than guessed at.
    (void)x; (void)y; (void)w; (void)h; (void)data;
}

void LT7683::setActiveWindow(std::uint16_t x0, std::uint16_t y0,
                             std::uint16_t x1, std::uint16_t y1) {
    writeReg16(AWUL_X0, x0);
    writeReg16(AWUL_Y0, static_cast<std::uint16_t>(y0 + vertOffset_));
    writeReg16(AW_WTH0, static_cast<std::uint16_t>(x1 - x0 + 1));
    writeReg16(AW_HT0, static_cast<std::uint16_t>(y1 - y0 + 1));
    // Cached for clearActiveWindow() to use directly, instead of reading
    // these back from the chip every single scroll.
    activeWindowX0Cached_ = x0;
    activeWindowY0Cached_ = static_cast<std::uint16_t>(y0 + vertOffset_);
    activeWindowWCached_ = static_cast<std::uint16_t>(x1 - x0 + 1);
    activeWindowHCached_ = static_cast<std::uint16_t>(y1 - y0 + 1);
}

void LT7683::clearActiveWindow() {
    // LT7683 has no single "clear active window" register command like
    // RA8875's MCLR -- uses the geometry engine's own rectangle fill
    // (rectHelper()) over the same region instead. Uses the cached
    // values setActiveWindow() itself already computed and wrote,
    // instead of reading them back from the chip.
    const std::uint16_t x0 = activeWindowX0Cached_;
    const std::uint16_t y0 = activeWindowY0Cached_;
    const std::uint16_t w = activeWindowWCached_;
    const std::uint16_t h = activeWindowHCached_;

    // Save the CURRENT foreground colour before the fill clobbers it --
    // rectHelper()'s fill uses whatever colour we pass it (black, here),
    // written to the same FGCR/FGCG/FGCB registers txtColor()'s
    // foreground half uses. Left unrestored, this used to silently leave
    // the hardware foreground colour at black after every clear -- which
    // is exactly what made Screen's very first characters invisible
    // (black-on-black): its constructor sets white via txtColor() *before*
    // calling clear() here, but draw_letter() never re-applies colour per
    // character (see its own comment), so whatever clear() left behind
    // silently became the "current" colour for everything drawn after.
    const std::uint8_t savedFgR = readReg(FGCR);
    const std::uint8_t savedFgG = readReg(FGCG);
    const std::uint8_t savedFgB = readReg(FGCB);
    rectHelper(static_cast<std::int16_t>(x0), static_cast<std::int16_t>(y0),
              static_cast<std::int16_t>(x0 + w - 1), static_cast<std::int16_t>(y0 + h - 1),
              0x0000, true);
    writeReg(FGCR, savedFgR);
    writeReg(FGCG, savedFgG);
    writeReg(FGCB, savedFgB);
}

void LT7683::beginMemoryWrite() {
    writeCmd(MRWDP);
}

void LT7683::rectHelper(std::int16_t x1, std::int16_t y1,
                        std::int16_t x2, std::int16_t y2,
                        std::uint16_t color, bool filled) {
    gfxMode();
    writeReg16(DLHSR0, static_cast<std::uint16_t>(x1));
    writeReg16(DLVSR0, static_cast<std::uint16_t>(y1 + vertOffset_));
    writeReg16(DLHER0, static_cast<std::uint16_t>(x2));
    writeReg16(DLVER0, static_cast<std::uint16_t>(y2 + vertOffset_));
    setColor(color);
    // DCR1 (REG[76h])'s "10b: Draw Rectangle" path -- proven reliable,
    // also used by roundRectHelper() for rounded rectangles. Coordinate
    // registers (DLHSR0-DLVER0, REG[68h]-[6Fh]) set above are the same
    // regardless of which control register triggers the fill.
    const std::uint8_t shape = 0x02 << 4;  // bit[5:4]=10b: Draw Rectangle
    writeReg(DCR1, static_cast<std::uint8_t>(0x80 | (filled ? 0x40 : 0x00) | shape));
    waitPoll(DCR1, 0x80);
}

void LT7683::rect(std::int16_t x, std::int16_t y, std::int16_t w, std::int16_t h,
                  std::uint16_t color) {
    rectHelper(x, y, static_cast<std::int16_t>(x + w - 1),
               static_cast<std::int16_t>(y + h - 1), color, false);
}

void LT7683::fillRect(std::int16_t x, std::int16_t y, std::int16_t w, std::int16_t h,
                      std::uint16_t color) {
    rectHelper(x, y, static_cast<std::int16_t>(x + w - 1),
               static_cast<std::int16_t>(y + h - 1), color, true);
}

void LT7683::fill(std::uint16_t color) {
    rectHelper(0, 0, static_cast<std::int16_t>(width_  - 1),
               static_cast<std::int16_t>(height_ - 1), color, true);
}

void LT7683::hline(std::int16_t x, std::int16_t y, std::int16_t w, std::uint16_t color) {
    line(x, y, static_cast<std::int16_t>(x + w - 1), y, color);
}

void LT7683::vline(std::int16_t x, std::int16_t y, std::int16_t h, std::uint16_t color) {
    line(x, y, x, static_cast<std::int16_t>(y + h - 1), color);
}

void LT7683::line(std::int16_t x1, std::int16_t y1,
                  std::int16_t x2, std::int16_t y2, std::uint16_t color) {
    gfxMode();
    writeReg16(DLHSR0, static_cast<std::uint16_t>(x1));
    writeReg16(DLVSR0, static_cast<std::uint16_t>(y1 + vertOffset_));
    writeReg16(DLHER0, static_cast<std::uint16_t>(x2));
    writeReg16(DLVER0, static_cast<std::uint16_t>(y2 + vertOffset_));
    setColor(color);
    // DCR0: shape=0000b (Line)
    writeReg(DCR0, 0x80);
    waitPoll(DCR0, 0x80);
}

void LT7683::circleHelper(std::int16_t x, std::int16_t y, std::uint16_t radius,
                          std::uint16_t color, bool filled) {
    ellipseHelper(x, y, radius, radius, color, filled);
}

void LT7683::ellipseHelper(std::int16_t x, std::int16_t y,
                           std::uint16_t ha, std::uint16_t va,
                           std::uint16_t color, bool filled) {
    gfxMode();
    writeReg16(ELL_A0, ha);
    writeReg16(ELL_B0, va);
    writeReg16(DEHR0, static_cast<std::uint16_t>(x));
    writeReg16(DEVR0, static_cast<std::uint16_t>(y + vertOffset_));
    setColor(color);
    // DCR1 (REG[76h]): bit7=start, bit6=fill, bit[5:4]=00b (Circle/Ellipse)
    writeReg(DCR1, static_cast<std::uint8_t>(0x80 | (filled ? 0x40 : 0x00)));
    waitPoll(DCR1, 0x80);
}

void LT7683::roundRectHelper(std::int16_t x1, std::int16_t y1,
                             std::int16_t x2, std::int16_t y2,
                             std::uint16_t rr, std::uint16_t color, bool filled) {
    gfxMode();
    // Rounded-rectangle needs both the bounding corner points (shared
    // with Line/Rectangle's own registers) AND the corner radius --
    // confirmed by this datasheet's own worked procedure (section 11.6):
    // "Host has to set the Start Point Coordinates (REG[68h~6Bh]), End
    // Point Coordinates (REG[6Ch~6Fh]), the Rounded Radius (REG[77h~7Ah])".
    writeReg16(DLHSR0, static_cast<std::uint16_t>(x1));
    writeReg16(DLVSR0, static_cast<std::uint16_t>(y1 + vertOffset_));
    writeReg16(DLHER0, static_cast<std::uint16_t>(x2));
    writeReg16(DLVER0, static_cast<std::uint16_t>(y2 + vertOffset_));
    writeReg16(ELL_A0, rr);
    writeReg16(ELL_B0, rr);
    setColor(color);
    // DCR1: bit[5:4]=11b (Rounded-Rectangle)
    writeReg(DCR1, static_cast<std::uint8_t>(0x80 | (filled ? 0x40 : 0x00) | 0x30));
    waitPoll(DCR1, 0x80);
}

void LT7683::fillRoundRect(std::int16_t x, std::int16_t y, std::int16_t w, std::int16_t h,
                           std::uint16_t r, std::uint16_t color) {
    // Datasheet section 6.6 ("Drawing Rounded-Rectangle"), Note1/Note2:
    // "DLHER-DLHSR must [be] large[r] than 2*R1+1" (same for Y/R2) --
    // i.e. w/h must be STRICTLY GREATER than 2*r+1, not merely >= 2*r.
    // Confirmed on real hardware that violating this (even by exactly
    // 1 -- e.g. w=h=20 with r=10, where 2*10+1=21 and 20 is not greater)
    // produces undefined behaviour: a status-LED circle at exactly this
    // boundary rendered as a long streak extending to the bottom of the
    // panel instead of a small circle. The old clamp here
    // (`rr*2 > w` before reducing rr) missed the "+1", so this exact
    // boundary case slipped through unclamped.
    std::uint16_t rr = r;
    if (static_cast<std::int16_t>(rr * 2 + 1) >= w) rr = static_cast<std::uint16_t>((w - 1) / 2);
    if (static_cast<std::int16_t>(rr * 2 + 1) >= h) rr = static_cast<std::uint16_t>((h - 1) / 2);
    if (rr == 0) {
        fillRect(x, y, w, h, color);
        return;
    }
    roundRectHelper(x, y, static_cast<std::int16_t>(x + w - 1),
                    static_cast<std::int16_t>(y + h - 1), rr, color, true);
}

void LT7683::roundRect(std::int16_t x, std::int16_t y, std::int16_t w, std::int16_t h,
                       std::uint16_t r, std::uint16_t color) {
    // Same clamp fix as fillRoundRect() above -- see its own comment.
    std::uint16_t rr = r;
    if (static_cast<std::int16_t>(rr * 2 + 1) >= w) rr = static_cast<std::uint16_t>((w - 1) / 2);
    if (static_cast<std::int16_t>(rr * 2 + 1) >= h) rr = static_cast<std::uint16_t>((h - 1) / 2);
    if (rr == 0) {
        rect(x, y, w, h, color);
        return;
    }
    roundRectHelper(x, y, static_cast<std::int16_t>(x + w - 1),
                    static_cast<std::int16_t>(y + h - 1), rr, color, false);
}

void LT7683::circle(std::int16_t x, std::int16_t y, std::uint16_t r, std::uint16_t color) {
    circleHelper(x, y, r, color, false);
}

void LT7683::fillCircle(std::int16_t x, std::int16_t y, std::uint16_t r, std::uint16_t color) {
    circleHelper(x, y, r, color, true);
}

void LT7683::ellipse(std::int16_t x, std::int16_t y, std::uint16_t rx, std::uint16_t ry, std::uint16_t color) {
    ellipseHelper(x, y, rx, ry, color, false);
}

void LT7683::fillEllipse(std::int16_t x, std::int16_t y, std::uint16_t rx, std::uint16_t ry, std::uint16_t color) {
    ellipseHelper(x, y, rx, ry, color, true);
}

// Confirmed against RA8876_Lite::drawTriangle()/drawTriangleFill() --
// same DCR0 (REG[67h]) register, same shape bits (0001b=Triangle,
// matching the 0x82/0xA2 opcodes their reference uses: bit7=start,
// bit5=fill, bit[4:1]=0001b).
void LT7683::triangleHelper(std::int16_t x0, std::int16_t y0, std::int16_t x1, std::int16_t y1,
                            std::int16_t x2, std::int16_t y2, std::uint16_t color, bool filled) {
    gfxMode();
    writeReg16(DLHSR0, static_cast<std::uint16_t>(x0));
    writeReg16(DLVSR0, static_cast<std::uint16_t>(y0 + vertOffset_));
    writeReg16(DLHER0, static_cast<std::uint16_t>(x1));
    writeReg16(DLVER0, static_cast<std::uint16_t>(y1 + vertOffset_));
    writeReg16(DTPH0, static_cast<std::uint16_t>(x2));
    writeReg16(DTPV0, static_cast<std::uint16_t>(y2 + vertOffset_));
    setColor(color);
    const std::uint8_t shape = 0x01 << 1;
    writeReg(DCR0, static_cast<std::uint8_t>(0x80 | (filled ? 0x20 : 0x00) | shape));
    waitPoll(DCR0, 0x80);
}

void LT7683::triangle(std::int16_t x0, std::int16_t y0, std::int16_t x1, std::int16_t y1,
                      std::int16_t x2, std::int16_t y2, std::uint16_t color) {
    triangleHelper(x0, y0, x1, y1, x2, y2, color, false);
}

void LT7683::fillTriangle(std::int16_t x0, std::int16_t y0, std::int16_t x1, std::int16_t y1,
                          std::int16_t x2, std::int16_t y2, std::uint16_t color) {
    triangleHelper(x0, y0, x1, y1, x2, y2, color, true);
}

// -----------------------------------------------------------------------
// Deferred: 2-layer/PIP config, CGRAM custom characters -- see LT7683.hpp's
// header comment for why these are stubbed rather than guessed at.
// -----------------------------------------------------------------------

void LT7683::set8Bpp() {
    // TODO(LT7683): map to whatever this project actually needs 8bpp mode
    // for on RA8875 before implementing -- CCR (REG[01h]) bit[4:3]
    // controls LCD interface bpp, but the *purpose* this serves elsewhere
    // in the codebase needs checking first.
}

void LT7683::set2LayerConfig() {
    // TODO(LT7683): see LT7683.hpp's header comment -- LT7683's PIP/
    // multi-buffer model doesn't map directly onto RA8875's simple
    // 2-layer overlay register (DPCTR bit7). Deferred until it's clear
    // what capability the project actually relies on this call for.
}

void LT7683::uploadCgramChar(std::uint8_t ascii, const std::uint8_t bitmap[16]) {
    // TODO(LT7683): LT7683's User-defined Character Graphic (UCG, section
    // 13.2) uses a differently-organised CGRAM (REG[CCh]-[DEh], per-size
    // data formats for 8x16/12x24/16x32) than RA8875's simpler CGRAM
    // upload. Given LT7683 already has larger built-in CGROM fonts than
    // RA8875, check whether the HP-41 custom glyph set (hp82163_font.hpp)
    // is even still needed here, or whether a built-in font size covers
    // it, before implementing this from scratch.
    (void)ascii;
    (void)bitmap;
}

}  // namespace hipi
