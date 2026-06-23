// SPDX-License-Identifier: MIT
//
// RA8875 LCD driver — C++17 port.
// Original MicroPython Display class: J. Chilla, March 2026.

#include "RA8875.hpp"

#include <cstring>

namespace hipi {

// -----------------------------------------------------------------------
// Construction & init
// -----------------------------------------------------------------------

RA8875::RA8875(RA8875Transport& t,
               std::uint16_t width,
               std::uint16_t height,
               bool start_on)
    : t_(t),
      width_(width),
      height_(height),
      vertOffset_(0),
      adcClk_(0x02),  // TPCR0_ADCCLK_DIV4
      txtScale_(0),
      mode_(nullptr) {
    (void)start_on;  // forwarded to begin()
}

void RA8875::begin() {
    begin(font, FONT_CHAR_COUNT);
}

void RA8875::begin(const std::uint8_t (*font)[FONT_BYTES_PER_CHAR],
                   std::size_t fontChars) {
    // Some panels report 480x80 but are actually 480x82 — the share.py code
    // matches the MicroPython Display.__init__ quirk.
    if (width_ == 480 && height_ == 80) {
        height_ = 82;
    }

    reset();

    // If a previously-configured controller is connected, skip the PLL/timing
    // re-init sequence (matches the early-return in the MicroPython original).
    if (readReg(0) == 0x75) {
        return;
    }

    std::uint8_t pixclk = 0;
    std::uint8_t hsync_nondisp = 0, hsync_start = 0, hsync_pw = 0;
    std::uint8_t vsync_nondisp = 0, vsync_start = 0, vsync_pw = 0;
    std::uint8_t hsync_finetune = 0;

    if (width_ == 480 && height_ == 82) {
        vertOffset_ = 190;
    }

    if (width_ == 800 && height_ == 480) {
        pixclk         = PCSR_PDATL | PCSR_2CLK;
        hsync_nondisp  = 26;
        hsync_start    = 32;
        hsync_pw       = 96;
        vsync_nondisp  = 32;
        vsync_start    = 23;
        vsync_pw       = 2;
        adcClk_        = 0x04;  // TPCR0_ADCCLK_DIV16
    } else if (width_ == 480 && (height_ == 272 || height_ == 128 || height_ == 82)) {
        pixclk         = PCSR_PDATL | PCSR_4CLK;
        hsync_nondisp  = 10;
        hsync_start    = 8;
        hsync_pw       = 48;
        vsync_nondisp  = 3;
        vsync_start    = 8;
        vsync_pw       = 10;
    } else {
        // unsupported size — fall back to 800x480 timing
        pixclk         = PCSR_PDATL | PCSR_2CLK;
        hsync_nondisp  = 26;
        hsync_start    = 32;
        hsync_pw       = 96;
        vsync_nondisp  = 32;
        vsync_start    = 23;
        vsync_pw       = 2;
        adcClk_        = 0x04;
    }

    pllInit();
    writeReg(SYSR, SYSR_16BPP | SYSR_MCU8);
    writeReg(PCSR, pixclk);
    t_.delayMs(1);

    // Horizontal
    writeReg(HDWR,   static_cast<std::uint8_t>((width_ / 8) - 1));
    writeReg(HNDFTR, static_cast<std::uint8_t>(HNDFTR_DE_HIGH + hsync_finetune));
    writeReg(HNDR,   static_cast<std::uint8_t>((hsync_nondisp - hsync_finetune - 2) / 8));
    writeReg(HSTR,   static_cast<std::uint8_t>((hsync_start / 8) - 1));
    writeReg(HPWR,   static_cast<std::uint8_t>(HPWR_LOW + (hsync_pw / 8 - 1)));

    // Vertical
    const std::uint16_t vdh = (height_ - 1 + vertOffset_) & 0x1FF;
    writeReg(VDHR0,  static_cast<std::uint8_t>(vdh & 0xFF));
    writeReg(VDHR1,  static_cast<std::uint8_t>(vdh >> 8));
    writeReg(VNDR0,  static_cast<std::uint8_t>(vsync_nondisp - 1));
    writeReg(VNDR1,  static_cast<std::uint8_t>(vsync_nondisp >> 8));
    writeReg(VSTR0,  static_cast<std::uint8_t>(vsync_start - 1));
    writeReg(VSTR1,  static_cast<std::uint8_t>(vsync_start >> 8));
    writeReg(VPWR,   static_cast<std::uint8_t>(VPWR_LOW + vsync_pw - 1));

    // Active window X
    writeReg(HSAW0, 0);
    writeReg(HSAW1, 0);
    const std::uint16_t hex_ = (width_ - 1) & 0x1FF;
    writeReg(HEAW0, static_cast<std::uint8_t>(hex_ & 0xFF));
    writeReg(HEAW1, static_cast<std::uint8_t>(hex_ >> 8));

    // Active window Y
    writeReg(VSAW0, static_cast<std::uint8_t>(vertOffset_ & 0xFF));
    writeReg(VSAW1, static_cast<std::uint8_t>(vertOffset_ >> 8));
    const std::uint16_t vex = (height_ - 1 + vertOffset_) & 0x1FF;
    writeReg(VEAW0, static_cast<std::uint8_t>(vex & 0xFF));
    writeReg(VEAW1, static_cast<std::uint8_t>(vex >> 8));

    // Clear
    writeReg(MCLR, MCLR_START | MCLR_FULL);
    t_.delayMs(500);

    turnOn(true);
    gpiox(true);
    pwm1Config(true, PWM_CLK_DIV1024);
    brightness(255);

    // CGRAM font upload — mirror share.py setup.
    if (font != nullptr && fontChars > 0) {
        gfxMode();
        for (std::size_t i = 0; i < fontChars; ++i) {
            uploadCgramChar(static_cast<std::uint8_t>(FONT_FIRST_ASCII + i), font[i]);
        }
        // restore MWCR0 = 0 (gfx mode, cursor off)
        writeReg(MWCR0, 0x00);
    }
}

void RA8875::pllInit() {
    writeReg(PLLC1, static_cast<std::uint8_t>(0x00 + 11));  // PLLC1_PLLDIV1 + 11
    t_.delayMs(1);
    writeReg(PLLC2, 0x02);                                   // PLLC2_DIV4
    t_.delayMs(1);
}

// -----------------------------------------------------------------------
// HIPI-specific configuration
// -----------------------------------------------------------------------

void RA8875::set8Bpp() {
    // SYSR_8BPP overrides the 16BPP default set in begin().
    writeReg(SYSR, SYSR_8BPP);
}

void RA8875::set2LayerConfig() {
    // DSPCTR register (0x20), bit 7 = two-layer mode.
    writeReg(0x20, 0x80);
}

void RA8875::uploadCgramChar(std::uint8_t ascii, const std::uint8_t bitmap[16]) {
    // Matches the per-character loop in share.py:
    //   display._write_reg(0x23, i+32)   ; CGRAM character index
    //   display._write_reg(0x21, 0x00)   ; clear FNCR0 (DDRAM/text mode off)
    //   display._write_data((display._read_reg(0x41) & ~(1<<3)) | (1<<2))
    //   display._write_cmd(MRWC)
    //   display._write_data(bytearray(f[i]), True)
    writeReg(0x23, ascii);
    writeReg(0x21, 0x00);
    const std::uint8_t mwcr0 = readReg(0x41);
    writeData(static_cast<std::uint8_t>((mwcr0 & ~(1 << 3)) | (1 << 2)));
    writeCmd(MRWC);
    writeData(bitmap, 16);
}

// -----------------------------------------------------------------------
// Register access
// -----------------------------------------------------------------------

void RA8875::writeReg(std::uint8_t cmd, std::uint8_t data) {
    writeCmd(cmd);
    writeData(data);
}

void RA8875::writeReg(std::uint8_t cmd, const std::uint8_t* data, std::size_t len) {
    writeCmd(cmd);
    writeData(data, len);
}

void RA8875::writeReg16(std::uint8_t cmd, std::uint16_t data) {
    writeCmd(cmd);
    writeData(static_cast<std::uint8_t>(data & 0xFF));
    writeCmd(static_cast<std::uint8_t>(cmd + 1));
    writeData(static_cast<std::uint8_t>((data >> 8) & 0xFF));
}

void RA8875::writeCmd(std::uint8_t cmd) {
    const std::uint8_t tx[2] = { CMDWR, cmd };
    t_.csLow();
    t_.spiTransfer(tx, nullptr, 2);
    t_.csHigh();
}

void RA8875::writeData(std::uint8_t data) {
    const std::uint8_t tx[2] = { DATWR, data };
    t_.csLow();
    t_.spiTransfer(tx, nullptr, 2);
    t_.csHigh();
}

void RA8875::writeData(const std::uint8_t* data, std::size_t len) {
    if (len == 0) return;
    t_.csLow();
    t_.spiTransfer(&DATWR, nullptr, 1);
    t_.spiTransfer(data, nullptr, len);
    t_.csHigh();
}

std::uint8_t RA8875::readReg(std::uint8_t cmd) {
    writeCmd(cmd);
    return readData();
}

std::uint8_t RA8875::readData() {
    std::uint8_t rx[1] = { 0 };
    t_.csLow();
    t_.spiTransfer(&DATRD, nullptr, 1);
    t_.spiTransfer(nullptr, rx, 1);
    t_.csHigh();
    return rx[0];
}

std::uint8_t RA8875::readStatus() {
    std::uint8_t rx[1] = { 0 };
    t_.csLow();
    t_.spiTransfer(&CMDRD, nullptr, 1);
    t_.spiTransfer(nullptr, rx, 1);
    t_.csHigh();
    return rx[0];
}

void RA8875::waitPoll(std::uint8_t reg, std::uint8_t mask) {
    // 50 ms timeout, matching the MicroPython reference.
    for (int i = 0; i < 50; ++i) {
        t_.delayMs(1);
        if ((readReg(reg) & mask) == 0) return;
    }
}

void RA8875::waitStatus(std::uint8_t mask) {
    for (int i = 0; i < 50; ++i) {
        t_.delayMs(1);
        if ((readStatus() & mask) == 0) return;
    }
}

// -----------------------------------------------------------------------
// BTE / power / PWM / reset
// -----------------------------------------------------------------------

void RA8875::BTE(std::uint8_t  opcode,
                 std::uint16_t x1, std::uint16_t y1,
                 std::uint16_t w,  std::uint16_t h,
                 std::uint16_t x0, std::uint16_t y0) {
    writeReg16(0x54, x0);
    writeReg16(0x56, y0);
    writeReg16(0x58, x1);
    writeReg16(0x5A, y1);
    writeReg16(0x5C, w);
    writeReg16(0x5E, h);
    writeReg(0x51, opcode);
    writeReg(0x50, 0x80);
    waitStatus();
}

void RA8875::turnOn(bool on) {
    if (on) {
        writeReg(PWRR, static_cast<std::uint8_t>(PWRR_NORMAL | PWRR_DISPON));
    } else {
        writeReg(PWRR, static_cast<std::uint8_t>(PWRR_NORMAL | PWRR_DISPOFF));
    }
}

void RA8875::reset() {
    t_.rstLow();
    t_.delayMs(100);
    t_.rstHigh();
    t_.delayMs(100);
}

void RA8875::softReset() {
    writeReg(PWRR, PWRR_SOFTRESET);
    writeData(PWRR_NORMAL);
    t_.delayMs(1);
}

void RA8875::sleep(bool s) {
    writeReg(PWRR, s ? PWRR_DISPOFF : static_cast<std::uint8_t>(PWRR_DISPOFF | PWRR_SLEEP));
}

void RA8875::brightness(std::uint8_t level) {
    writeReg(P1DCR, level);
}

void RA8875::gpiox(bool on) {
    writeReg(GPIOX, on ? 1 : 0);
}

void RA8875::pwm1Config(bool on, std::uint8_t clock) {
    const std::uint8_t enable = on ? 0x80 : 0x00;
    writeReg(P1CR, static_cast<std::uint8_t>(enable | (clock & 0x07)));
}

// -----------------------------------------------------------------------
// Mode switch
// -----------------------------------------------------------------------

void RA8875::gfxMode() {
    if (mode_ != nullptr && std::strcmp(mode_, "gfx") == 0) return;
    writeData(static_cast<std::uint8_t>(readReg(MWCR0) & ~MWCR0_TXTMODE));
    mode_ = "gfx";
}

void RA8875::txtMode() {
    if (mode_ != nullptr && std::strcmp(mode_, "txt") == 0) return;
    writeData(static_cast<std::uint8_t>(readReg(MWCR0) | MWCR0_TXTMODE));
    writeData(static_cast<std::uint8_t>(readReg(FNCR1) & ~((1 << 7) | (1 << 5))));
    mode_ = "txt";
}

// -----------------------------------------------------------------------
// Colour + cursor + text
// -----------------------------------------------------------------------

void RA8875::setColor(std::uint16_t color) {
    writeReg(0x63, static_cast<std::uint8_t>((color & 0xF800) >> 11));
    writeReg(0x64, static_cast<std::uint8_t>((color & 0x07E0) >> 5));
    writeReg(0x65, static_cast<std::uint8_t>( color & 0x001F));
}

void RA8875::setBgColor(std::uint16_t color) {
    writeReg(0x60, static_cast<std::uint8_t>((color & 0xF800) >> 11));
    writeReg(0x61, static_cast<std::uint8_t>((color & 0x07E0) >> 5));
    writeReg(0x62, static_cast<std::uint8_t>( color & 0x001F));
}

void RA8875::setxy(std::uint16_t x, std::uint16_t y) {
    gfxMode();
    writeReg16(CURH0, x);
    writeReg16(CURV0, static_cast<std::uint16_t>(y + vertOffset_));
}

void RA8875::txtSetCursor(std::uint16_t x, std::uint16_t y) {
    txtMode();
    writeReg16(0x2A, x);
    writeReg16(0x2C, static_cast<std::uint16_t>(y + vertOffset_));
}

void RA8875::txtColor(std::uint16_t fg, std::uint16_t bg) {
    setColor(fg);
    setBgColor(bg);
    writeData(static_cast<std::uint8_t>(readReg(FNCR1) & ~(1 << 6)));
}

void RA8875::txtTrans(std::uint16_t color) {
    txtMode();
    setColor(color);
    writeData(static_cast<std::uint8_t>(readReg(FNCR1) | (1 << 6)));
}

void RA8875::txtSize(std::uint8_t scale) {
    txtMode();
    if (scale > 3) scale = 3;
    writeData(static_cast<std::uint8_t>((readReg(FNCR1) & ~0x0F) | ((scale << 2) | scale)));
    txtScale_ = scale;
}

void RA8875::txtWrite(const char* s) {
    txtMode();
    writeCmd(MRWC);
    for (const char* p = s; *p; ++p) {
        writeData(static_cast<std::uint8_t>(*p));
        if (txtScale_ > 0) t_.delayMs(1);
    }
}

void RA8875::txtWriteChar(std::uint8_t c) {
    txtMode();
    writeCmd(MRWC);
    writeData(c);
    if (txtScale_ > 0) t_.delayMs(1);
}

// -----------------------------------------------------------------------
// Primitives
// -----------------------------------------------------------------------

void RA8875::pixel(std::int16_t x, std::int16_t y, std::uint16_t color) {
    setxy(static_cast<std::uint16_t>(x), static_cast<std::uint16_t>(y));
    const std::uint8_t data[2] = {
        static_cast<std::uint8_t>(color >> 8),
        static_cast<std::uint8_t>(color & 0xFF)
    };
    writeReg(MRWC, data, 2);
}

void RA8875::fillRect(std::int16_t x, std::int16_t y, std::int16_t w, std::int16_t h,
                      std::uint16_t color) {
    rectHelper(x, y, static_cast<std::int16_t>(x + w - 1),
               static_cast<std::int16_t>(y + h - 1), color, true);
}

void RA8875::rect(std::int16_t x, std::int16_t y, std::int16_t w, std::int16_t h,
                  std::uint16_t color) {
    rectHelper(x, y, static_cast<std::int16_t>(x + w - 1),
               static_cast<std::int16_t>(y + h - 1), color, false);
}

void RA8875::fill(std::uint16_t color) {
    rectHelper(0, 0, static_cast<std::int16_t>(width_  - 1),
               static_cast<std::int16_t>(height_ - 1), color, true);
}

void RA8875::hline(std::int16_t x, std::int16_t y, std::int16_t w, std::uint16_t color) {
    line(x, y, static_cast<std::int16_t>(x + w - 1), y, color);
}

void RA8875::vline(std::int16_t x, std::int16_t y, std::int16_t h, std::uint16_t color) {
    line(x, y, x, static_cast<std::int16_t>(y + h - 1), color);
}

void RA8875::line(std::int16_t x1, std::int16_t y1,
                  std::int16_t x2, std::int16_t y2, std::uint16_t color) {
    gfxMode();
    writeReg16(0x91, static_cast<std::uint16_t>(x1));
    writeReg16(0x93, static_cast<std::uint16_t>(y1 + vertOffset_));
    writeReg16(0x95, static_cast<std::uint16_t>(x2));
    writeReg16(0x97, static_cast<std::uint16_t>(y2 + vertOffset_));
    setColor(color);
    writeReg(DCR, 0x80);
    waitPoll(DCR, DCR_LNSQTR_STATUS);
}

void RA8875::rectHelper(std::int16_t x1, std::int16_t y1,
                        std::int16_t x2, std::int16_t y2,
                        std::uint16_t color, bool filled) {
    gfxMode();
    writeReg16(0x91, static_cast<std::uint16_t>(x1));
    writeReg16(0x93, static_cast<std::uint16_t>(y1 + vertOffset_));
    writeReg16(0x95, static_cast<std::uint16_t>(x2));
    writeReg16(0x97, static_cast<std::uint16_t>(y2 + vertOffset_));
    setColor(color);
    writeReg(DCR, filled ? 0xB0 : 0x90);
    waitPoll(DCR, DCR_LNSQTR_STATUS);
}

void RA8875::circleHelper(std::int16_t x, std::int16_t y, std::uint16_t radius,
                          std::uint16_t color, bool filled) {
    gfxMode();
    writeReg16(0x99, static_cast<std::uint16_t>(x));
    writeReg16(0x9B, static_cast<std::uint16_t>(y + vertOffset_));
    writeReg(0x9D, static_cast<std::uint8_t>(radius));
    setColor(color);
    writeReg(DCR, static_cast<std::uint8_t>(DCR_CIRC_START |
                                            (filled ? DCR_FILL : DCR_NOFILL)));
    waitPoll(DCR, DCR_CIRC_STATUS);
}

void RA8875::ellipseHelper(std::int16_t x, std::int16_t y,
                           std::uint16_t ha, std::uint16_t va,
                           std::uint16_t color, bool filled) {
    gfxMode();
    writeReg16(0xA5, static_cast<std::uint16_t>(x));
    writeReg16(0xA7, static_cast<std::uint16_t>(y + vertOffset_));
    writeReg16(0xA1, ha);
    writeReg16(0xA3, va);
    setColor(color);
    writeReg(ELLIPSE, filled ? 0xC0 : 0x80);
    waitPoll(ELLIPSE, ELLIPSE_STATUS);
}

void RA8875::curveHelper(std::int16_t x, std::int16_t y,
                         std::uint16_t ha, std::uint16_t va,
                         std::uint8_t part, std::uint16_t color, bool filled) {
    gfxMode();
    writeReg16(0xA5, static_cast<std::uint16_t>(x));
    writeReg16(0xA7, static_cast<std::uint16_t>(y + vertOffset_));
    writeReg16(0xA1, ha);
    writeReg16(0xA3, va);
    setColor(color);
    writeReg(ELLIPSE, static_cast<std::uint8_t>((filled ? 0xD0 : 0x90) | (part & 0x03)));
    waitPoll(ELLIPSE, ELLIPSE_STATUS);
}

void RA8875::triangleHelper(std::int16_t x1, std::int16_t y1,
                            std::int16_t x2, std::int16_t y2,
                            std::int16_t x3, std::int16_t y3,
                            std::uint16_t color, bool filled) {
    gfxMode();
    writeReg16(0x91, static_cast<std::uint16_t>(x1));
    writeReg16(0x93, static_cast<std::uint16_t>(y1 + vertOffset_));
    writeReg16(0x95, static_cast<std::uint16_t>(x2));
    writeReg16(0x97, static_cast<std::uint16_t>(y2 + vertOffset_));
    writeReg16(0xA9, static_cast<std::uint16_t>(x3));
    writeReg16(0xAB, static_cast<std::uint16_t>(y3 + vertOffset_));
    setColor(color);
    writeReg(DCR, filled ? 0xA1 : 0x81);
    waitPoll(DCR, DCR_LNSQTR_STATUS);
}

}  // namespace hipi

