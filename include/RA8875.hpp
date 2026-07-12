// SPDX-License-Identifier: MIT
//
// RA8875 LCD driver — ported from MicroPython / CircuitPython to C++17.
//
// Original MicroPython Display class: J. Chilla, March 2026, based on
// https://github.com/adafruit/Adafruit_CircuitPython_RA8875

#pragma once

#include "RA8875Transport.hpp"
#include "hp82163_font.hpp"
#include <cstdint>

namespace hp82163 {

class RA8875 {
public:
    // -----------------------------------------------------------------------
    // Constants — register addresses and bit flags.  Names match the
    // MicroPython `Display` class so the port stays grep-able.
    // -----------------------------------------------------------------------

    // SPI command/data bytes
    static constexpr std::uint8_t DATWR = 0x00;  // Data write
    static constexpr std::uint8_t DATRD = 0x40;  // Data read
    static constexpr std::uint8_t CMDWR = 0x80;  // Command write
    static constexpr std::uint8_t CMDRD = 0xC0;  // Status read

    // Register addresses
    static constexpr std::uint8_t PWRR   = 0x01;
    static constexpr std::uint8_t MRWC   = 0x02;
    static constexpr std::uint8_t PCSR   = 0x04;
    static constexpr std::uint8_t SYSR   = 0x10;
    static constexpr std::uint8_t HDWR   = 0x14;
    static constexpr std::uint8_t HNDFTR = 0x15;
    static constexpr std::uint8_t HNDR   = 0x16;
    static constexpr std::uint8_t HSTR   = 0x17;
    static constexpr std::uint8_t HPWR   = 0x18;
    static constexpr std::uint8_t VDHR0  = 0x19;
    static constexpr std::uint8_t VDHR1  = 0x1A;
    static constexpr std::uint8_t VNDR0  = 0x1B;
    static constexpr std::uint8_t VNDR1  = 0x1C;
    static constexpr std::uint8_t VSTR0  = 0x1D;
    static constexpr std::uint8_t VSTR1  = 0x1E;
    static constexpr std::uint8_t VPWR   = 0x1F;
    static constexpr std::uint8_t FNCR0  = 0x21;
    static constexpr std::uint8_t FNCR1  = 0x22;
    static constexpr std::uint8_t HSAW0  = 0x30;
    static constexpr std::uint8_t HSAW1  = 0x31;
    static constexpr std::uint8_t VSAW0  = 0x32;
    static constexpr std::uint8_t VSAW1  = 0x33;
    static constexpr std::uint8_t HEAW0  = 0x34;
    static constexpr std::uint8_t HEAW1  = 0x35;
    static constexpr std::uint8_t VEAW0  = 0x36;
    static constexpr std::uint8_t VEAW1  = 0x37;
    static constexpr std::uint8_t MWCR0  = 0x40;
    static constexpr std::uint8_t CURH0  = 0x46;
    static constexpr std::uint8_t CURV0  = 0x48;
    static constexpr std::uint8_t DCR    = 0x90;
    static constexpr std::uint8_t ELLIPSE = 0xA0;
    static constexpr std::uint8_t P1CR   = 0x8A;
    static constexpr std::uint8_t P1DCR  = 0x8B;
    static constexpr std::uint8_t P2CR   = 0x8C;
    static constexpr std::uint8_t GPIOX  = 0xC7;
    static constexpr std::uint8_t MCLR   = 0x8E;
    static constexpr std::uint8_t PLLC1  = 0x88;
    static constexpr std::uint8_t PLLC2  = 0x89;

    // PWRR bits
    static constexpr std::uint8_t PWRR_DISPON    = 0x80;
    static constexpr std::uint8_t PWRR_DISPOFF   = 0x00;
    static constexpr std::uint8_t PWRR_SLEEP     = 0x02;
    static constexpr std::uint8_t PWRR_NORMAL    = 0x00;
    static constexpr std::uint8_t PWRR_SOFTRESET = 0x01;

    // SYSR bits
    static constexpr std::uint8_t SYSR_8BPP  = 0x00;
    static constexpr std::uint8_t SYSR_16BPP = 0x0C;
    static constexpr std::uint8_t SYSR_MCU8  = 0x00;
    static constexpr std::uint8_t SYSR_MCU16 = 0x03;

    // PCSR bits
    static constexpr std::uint8_t PCSR_PDATL = 0x80;
    static constexpr std::uint8_t PCSR_2CLK  = 0x01;
    static constexpr std::uint8_t PCSR_4CLK  = 0x02;

    // HNDFTR bits
    static constexpr std::uint8_t HNDFTR_DE_HIGH = 0x00;
    static constexpr std::uint8_t HNDFTR_DE_LOW  = 0x80;

    // HPWR / VPWR bits
    static constexpr std::uint8_t HPWR_LOW  = 0x00;
    static constexpr std::uint8_t HPWR_HIGH = 0x80;
    static constexpr std::uint8_t VPWR_LOW  = 0x00;
    static constexpr std::uint8_t VPWR_HIGH = 0x80;

    // MWCR0 mode bits
    static constexpr std::uint8_t MWCR0_GFXMODE = 0x00;
    static constexpr std::uint8_t MWCR0_TXTMODE = 0x80;

    // MCLR bits
    static constexpr std::uint8_t MCLR_START       = 0x80;
    static constexpr std::uint8_t MCLR_STOP        = 0x00;
    static constexpr std::uint8_t MCLR_FULL        = 0x00;
    static constexpr std::uint8_t MCLR_ACTIVE      = 0x40;

    // DCR bits
    static constexpr std::uint8_t DCR_LNSQTR_START  = 0x80;
    static constexpr std::uint8_t DCR_LNSQTR_STATUS = 0x80;
    static constexpr std::uint8_t DCR_CIRC_START    = 0x40;
    static constexpr std::uint8_t DCR_CIRC_STATUS   = 0x40;
    static constexpr std::uint8_t DCR_FILL          = 0x20;
    static constexpr std::uint8_t DCR_NOFILL        = 0x00;

    // ELLIPSE status
    static constexpr std::uint8_t ELLIPSE_STATUS = 0x80;

    // PWM
    static constexpr std::uint8_t PWM_CLK_DIV1024 = 0x02;

    enum class Layer : std::uint8_t { Layer1 = 0, Layer2 = 1 };

    enum class LayerMode : std::uint8_t {
        ShowLayer1     = 0b00,  // visa bara lager 1
        ShowLayer2     = 0b01,  // visa bara lager 2
        LightenOverlay = 0b10,  // visa OR av bada lagren (bra nar bakgrund = 0x00)
        Transparent    = 0b11,  // alfablandning enligt FLDR-registret
    };

    // -----------------------------------------------------------------------
    // Construction
    // -----------------------------------------------------------------------

    // Construct the driver.  Initialisation is deferred to `begin()` so
    // the caller can wire up the transport and decide on display options
    // before the controller is touched.
    //
    //   t           — transport implementation (must outlive this object)
    //   width       — display width in pixels (default 800)
    //   height      — display height in pixels (default 480)
    //   start_on    — turn display on during begin() (default true)
    RA8875(RA8875Transport& t,
           std::uint16_t     width  = 800,
           std::uint16_t     height = 480,
           bool              start_on = true);

    // Initialise the controller, PLL, timing, clear the screen, upload the
    // CGRAM font and turn on the backlight.  Must be called before any
    // drawing / Screen operation.
    //
    // Pass `nullptr` or omit the argument to skip the CGRAM upload (you can
    // do it later via uploadCgramChar()).  Pass a custom font to override
    // the bundled 8x16 HP82163 bitmap.
    void begin();
    void begin(const std::uint8_t (*font)[FONT_BYTES_PER_CHAR],
               std::size_t fontChars);

    // -----------------------------------------------------------------------
    // Low-level register access
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
    void waitStatus(std::uint8_t mask = 0x40);

    // Convenience: forward to the transport's millisecond delay.
    // Useful when an API method needs a delay that doesn't correspond to a
    // specific hardware operation (e.g. waiting for a memory clear).
    void spiDelayMs(std::uint32_t ms) { t_.delayMs(ms); }

    // -----------------------------------------------------------------------
    // High-level operations
    // -----------------------------------------------------------------------

    // Block-transfer engine.  Used for scrolling the screen.
    //   opcode — BTE function code
    //   x1,y1  — destination top-left
    //   w,h    — block size
    //   x0,y0  — source top-left (default 0,0)
    void BTE(std::uint8_t  opcode,
             std::uint16_t x1,  std::uint16_t y1,
             std::uint16_t w,   std::uint16_t h,
             std::uint16_t x0 = 0, std::uint16_t y0 = 0);

    void reset();
    void softReset();
    void turnOn(bool on);
    void sleep(bool s);
    void brightness(std::uint8_t level);

    void pllInit();

    // Mode switch
    void gfxMode();
    void txtMode();

    // Colour registers (RGB565)
    void setColor  (std::uint16_t c);
    void setBgColor(std::uint16_t c);

    // Cursor + text
    void setxy   (std::uint16_t x, std::uint16_t y);
    void txtSetCursor(std::uint16_t x, std::uint16_t y);
    void txtColor(std::uint16_t fg, std::uint16_t bg);
    void txtTrans(std::uint16_t color);
    void txtSize (std::uint8_t scale);
    void txtWrite(const char* s);
    void txtWriteChar(std::uint8_t c);

    // Primitives
    void pixel   (std::int16_t x, std::int16_t y, std::uint16_t color);
    void fillRect(std::int16_t x, std::int16_t y, std::int16_t w, std::int16_t h, std::uint16_t color);
    void rect    (std::int16_t x, std::int16_t y, std::int16_t w, std::int16_t h, std::uint16_t color);
    void fill    (std::uint16_t color);
    void hline   (std::int16_t x, std::int16_t y, std::int16_t w, std::uint16_t color);
    void vline   (std::int16_t x, std::int16_t y, std::int16_t h, std::uint16_t color);
    void line    (std::int16_t x1, std::int16_t y1, std::int16_t x2, std::int16_t y2, std::uint16_t color);
    void drawBitmap565(std::int16_t x, std::int16_t y,
                    std::uint16_t w, std::uint16_t h,
                    const std::uint16_t* data);
    void drawBitmap332(std::int16_t x, std::int16_t y,
                    std::uint16_t w, std::uint16_t h,
                    const std::uint8_t* data);   // 1 byte per pixel (RGB332)
    void setActiveWindow(std::uint16_t x0, std::uint16_t y0,
                     std::uint16_t x1, std::uint16_t y1);
    // -----------------------------------------------------------------------
    // HP82163-specific configuration (called by Screen / share.py setup)
    // -----------------------------------------------------------------------

    // Switch the controller to 8-bit colour depth (256 colours).
    void set8Bpp();

    // Enable RA8875 2-layer configuration (DSPCTR bit 7).  Used by
    // Screen::store()/recall() to keep a backup of the visible layer.
    void set2LayerConfig();
    void selectLayer(Layer layer);
    void setLayerMode(LayerMode mode);

    // Upload a single character to the CGRAM (char code, 16-row bitmap).
    // This is the per-character version of the share.py CGRAM loop.  The
    // bitmap is laid out with bit 7 = leftmost column.
    void uploadCgramChar(std::uint8_t ascii, const std::uint8_t bitmap[16]);

    // -----------------------------------------------------------------------
    // Accessors
    // -----------------------------------------------------------------------

    std::uint16_t width()      const { return width_; }
    std::uint16_t height()     const { return height_; }
    std::uint16_t vertOffset() const { return vertOffset_; }
    std::uint8_t  txtScale()   const { return txtScale_; }
    //const char*   mode()       const { return mode_; }

private:
    // ---- helpers ----
    void gpiox      (bool on);
    void pwm1Config (bool on, std::uint8_t clock);
    void rectHelper (std::int16_t x1, std::int16_t y1, std::int16_t x2, std::int16_t y2,
                     std::uint16_t color, bool filled);
    void circleHelper(std::int16_t x, std::int16_t y, std::uint16_t r, std::uint16_t color, bool filled);
    void ellipseHelper(std::int16_t x, std::int16_t y, std::uint16_t ha, std::uint16_t va,
                       std::uint16_t color, bool filled);
    void curveHelper (std::int16_t x, std::int16_t y, std::uint16_t ha, std::uint16_t va,
                      std::uint8_t part, std::uint16_t color, bool filled);
    void triangleHelper(std::int16_t x1, std::int16_t y1, std::int16_t x2, std::int16_t y2,
                        std::int16_t x3, std::int16_t y3, std::uint16_t color, bool filled);

    // ---- members ----
    RA8875Transport& t_;
    std::uint16_t width_;
    std::uint16_t height_;
    std::uint16_t vertOffset_;
    std::uint8_t  adcClk_;
    std::uint8_t  txtScale_;
    //const char*   mode_;        // "gfx", "txt", "fon", or nullptr
};


}  // namespace hp82163


