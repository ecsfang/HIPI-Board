// SPDX-License-Identifier: MIT
//
// LT7683 LCD driver -- see LT7683.hpp for the overview of what's grounded
// against the datasheet vs. estimated/pending real-hardware verification.

#include "LT7683.hpp"

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

    pllInit();

    // ---- SDRAM (Display RAM) init -- section 8 ----
    // REG[E0h]/[E1h] match this project's own earlier datasheet-table
    // values exactly. REG[E2h]/[E3h] (refresh interval) use EastRising's
    // own reference calculation instead of the datasheet's generic table,
    // since it's tailored to the *actual* MCLK we configure in pllInit()
    // (100MHz) rather than whatever MCLK the datasheet's own table
    // assumed: sdram_itv = ((64000000/8192)/(1000/60))-2 = 486 (0x1E6).
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
    writeReg(MACR, 0x40);   // REG[02h] bit6=1,bit7=0: 16bpp RGB565 mode 1;
                             // bit[2:1]=00: normal (left-right,top-down)
                             // memory write direction.
    writeReg(0x12, 0x80);   // REG[12h] bit7=1: PCLK falling edge (matches
                             // LCD_PCLK_Falling_Rising=1 in EastRising's
                             // reference); bit6 (Display On) left 0 here,
                             // set later by turnOn(true) below. Writing
                             // the full byte (rather than read-modify-
                             // write) also zeroes bit4/bit3/bit[2:0] --
                             // HSCAN left-to-right, VSCAN top-to-bottom,
                             // PDATA=RGB order -- matching the reference's
                             // own HSCAN_L_to_R()/VSCAN_T_to_B()/
                             // PDATA_Set_RGB() calls as a side effect.
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
    // Check immediately first, then delay if still busy -- see RA8875's
    // own waitPoll() for why (a real test confirmed sleeping before the
    // first check just wastes ~1ms on nearly every hardware draw call).
    for (int i = 0; i < 50; ++i) {
        if ((readReg(reg) & mask) == 0) return;
        t_.delayMs(1);
    }
}

void LT7683::waitStatus(std::uint8_t mask) {
    for (int i = 0; i < 50; ++i) {
        if ((readStatus() & mask) == 0) return;
        t_.delayMs(1);
    }
}

// -----------------------------------------------------------------------
// BTE
// -----------------------------------------------------------------------

void LT7683::BTE(std::uint8_t opcode,
                 std::uint16_t x1, std::uint16_t y1,
                 std::uint16_t w, std::uint16_t h,
                 std::uint16_t x0, std::uint16_t y0) {
    // Confirmed against RA8876_common::bteMemoryCopy()/bteMemoryCopyWithROP():
    // both check the core ISN'T ALREADY BUSY *before* touching any BTE
    // register at all, and both explicitly force graphic mode right
    // before triggering -- neither of which this project's own BTE() did
    // before now. If whatever drawing call preceded this (e.g. the
    // fillRect() calls drawing the squares being moved) left the core in
    // a not-quite-finished state by the time control returned here, an
    // immediate new trigger could race against that -- this closes that
    // gap the same way the proven-working reference does.
    for (int i = 0; i < 50; ++i) {
        if ((readStatus() & STSR_CORE_BUSY) == 0) break;
        t_.delayMs(1);
    }
    gfxMode();

    // S0/S1/DT each have a linear start ADDRESS plus a window X/Y offset
    // within that -- mirrors how the Main Window (MISA/MIW) and Active
    // Window (AWUL_X/Y) work together elsewhere in this chip. Since
    // everything here operates within the single full-panel framebuffer
    // starting at address 0, the start address is always 0 and the
    // "width" is always the panel's own stride -- only the X/Y window
    // offsets actually vary per call.
    writeReg16(S0_STR0, 0);
    writeReg16(static_cast<std::uint8_t>(S0_STR0 + 2), 0);
    writeReg16(S0_WTH0, width_);
    writeReg16(S0_X0, x0);
    writeReg16(S0_Y0, y0);

    // S1 deliberately left untouched -- confirmed against
    // RA8876_common::bteMemoryCopy() itself (the plain, no-ROP-exposed
    // variant, hardcoded to ROP=S0 same as this project always uses):
    // its S1 calls are explicitly commented out there, never written at
    // all. The earlier "S1 = all zeros" attempt (including S1 image
    // WIDTH=0) may well have been actively harmful -- a zero stride is
    // exactly the kind of value that could make internal address math
    // misbehave -- unlike simply never touching those registers.

    writeReg16(DT_STR0, 0);
    writeReg16(static_cast<std::uint8_t>(DT_STR0 + 2), 0);
    writeReg16(DT_WTH0, width_);
    writeReg16(DT_X0, x1);
    writeReg16(DT_Y0, y1);

    writeReg16(BLT_WTH0, w);
    writeReg16(BLT_HIG0, h);

    // REG[92h] (BLT_COLR): S0/S1/Destination colour depth -- confirmed
    // against EastRising's own reference AND RA8876_common, both of which
    // always set this before every BTE operation. bit[6:5]=01b (S0
    // 16bpp), bit[4:2]=001b (S1 16bpp), bit[1:0]=01b (Destination 16bpp).
    writeReg(BLT_COLR, 0x25);

    writeReg(BLT_CTRL1, opcode);   // REG[91h]: ROP[7:4] + operation code[3:0]
    // REG[90h] bit4: BTE enable/start -- blind write of just bit4,
    // matching RA8876_Lite's own lcdRegDataWrite(RA8876_BTE_CTRL0,
    // RA8876_BTE_ENABLE<<4) exactly.
    writeReg(BLT_CTRL0, 0x10);
    // Busy-wait via the STATUS register's Core Task Busy bit (bit3, same
    // STSR_CORE_BUSY used for the pre-check above and the post-reset/
    // SDRAM-ready checks in begin()) -- matching RA8876_common's own
    // check2dBusy(), which polls the dedicated STATUSREAD SPI command
    // rather than a normal register read of REG[90h] itself.
    for (int i = 0; i < 50; ++i) {
        if ((readStatus() & STSR_CORE_BUSY) == 0) break;
        t_.delayMs(1);
    }
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
    writeData(static_cast<std::uint8_t>(readReg(ICR) & ~0x04));  // ICR bit2=0: graphic mode
}

void LT7683::txtMode() {
    writeData(static_cast<std::uint8_t>(readReg(ICR) | 0x04));   // ICR bit2=1: text mode
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
    // See this method's own header comment -- nothing beyond text mode
    // is actually needed here on LT7683, unlike RA8875's REG[40h] write.
    txtMode();
}

void LT7683::txtColor(std::uint16_t fg, std::uint16_t bg) {
    setColor(fg);
    setBgColor(bg);
    // CCR1_TEXT (REG[CDh]) bit6: background transparency -- 0=opaque
    // (uses BGC*), matches RA8875's txtColor() semantics (opaque, not
    // transparent background).
    writeData(static_cast<std::uint8_t>(readReg(CCR1_TEXT) & ~(1 << 6)));
}

void LT7683::txtTrans(std::uint16_t color) {
    txtMode();
    setColor(color);
    writeData(static_cast<std::uint8_t>(readReg(CCR1_TEXT) | (1 << 6)));
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
        if (txtScale_ > 0) t_.delayMs(1);
    }
}

void LT7683::txtWriteChar(std::uint8_t c) {
    txtMode();
    writeCmd(MRWDP);
    writeData(c);
    if (txtScale_ > 0) t_.delayMs(1);
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

    for (std::uint16_t row = 0; row < h; ++row) {
        setxy(static_cast<std::uint16_t>(x), static_cast<std::uint16_t>(y + row));
        writeCmd(MRWDP);

        const std::uint16_t* src = data + static_cast<std::size_t>(row) * srcStride;
        for (std::uint16_t col = 0; col < drawWidth; ++col) {
            rowBuf[col * 2]     = static_cast<std::uint8_t>(src[col] >> 8);
            rowBuf[col * 2 + 1] = static_cast<std::uint8_t>(src[col] & 0xFF);
        }
        writeData(rowBuf, static_cast<std::size_t>(drawWidth) * 2);
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
}

void LT7683::clearActiveWindow() {
    // LT7683 has no single "clear active window" register command like
    // RA8875's MCLR. This used to go through a BTE Solid Fill operation
    // (opcode 1100b) -- but BTE on this chip never got past triggering
    // without freezing the panel (extensively bring-up-tested: every
    // register involved reads back correct, the core reports idle, yet
    // the panel stops updating and SDRAM keeps getting overwritten with
    // whatever colour was last set, surviving even a Display Off/On
    // cycle -- see display_boot_test.hpp's own notes on this). Since
    // Screen::clear() calls this on every construction and scroll, that
    // meant the display froze the moment a Screen object was created,
    // before any text was even drawn.
    //
    // Uses the same geometry-engine rectangle fill (DCR0, via
    // rectHelper()) that fillRect()/fill() already use -- confirmed
    // solid throughout every other test -- instead of BTE. Read back the
    // active window we just set so this stays correct regardless of what
    // it was last configured to.
    const std::uint16_t x0 = static_cast<std::uint16_t>(
        readReg(AWUL_X0) | (readReg(static_cast<std::uint8_t>(AWUL_X0 + 1)) << 8));
    const std::uint16_t y0 = static_cast<std::uint16_t>(
        readReg(AWUL_Y0) | (readReg(static_cast<std::uint8_t>(AWUL_Y0 + 1)) << 8));
    const std::uint16_t w = static_cast<std::uint16_t>(
        readReg(AW_WTH0) | (readReg(static_cast<std::uint8_t>(AW_WTH0 + 1)) << 8));
    const std::uint16_t h = static_cast<std::uint16_t>(
        readReg(AW_HT0) | (readReg(static_cast<std::uint8_t>(AW_HT0 + 1)) << 8));

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
    // DCR0 (REG[67h]): bit7=start, bit5=fill, bit[4:1]=shape (0010b=Rectangle)
    const std::uint8_t shape = 0x02 << 1;
    writeReg(DCR0, static_cast<std::uint8_t>(0x80 | (filled ? 0x20 : 0x00) | shape));
    waitPoll(DCR0, 0x80);
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
    std::uint16_t rr = r;
    if (static_cast<std::int16_t>(rr * 2) > w) rr = static_cast<std::uint16_t>(w / 2);
    if (static_cast<std::int16_t>(rr * 2) > h) rr = static_cast<std::uint16_t>(h / 2);
    if (rr == 0) {
        fillRect(x, y, w, h, color);
        return;
    }
    roundRectHelper(x, y, static_cast<std::int16_t>(x + w - 1),
                    static_cast<std::int16_t>(y + h - 1), rr, color, true);
}

void LT7683::roundRect(std::int16_t x, std::int16_t y, std::int16_t w, std::int16_t h,
                       std::uint16_t r, std::uint16_t color) {
    std::uint16_t rr = r;
    if (static_cast<std::int16_t>(rr * 2) > w) rr = static_cast<std::uint16_t>(w / 2);
    if (static_cast<std::int16_t>(rr * 2) > h) rr = static_cast<std::uint16_t>(h / 2);
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
