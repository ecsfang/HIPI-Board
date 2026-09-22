// SPDX-License-Identifier: MIT
//
// Raspberry Pico 2 (RP2350) SPI transport — implementation.
//
// Mirrors the MicroPython wiring from share.py:
//   spi0 = SPI(0, baudrate=6000000, sck=Pin(2), mosi=Pin(3), miso=Pin(0))
//   display = Display(spi0, cs=Pin(1, mode=Pin.OUT, value=1),
//                          rst=Pin(4, mode=Pin.OUT, value=1))
//
// Default constructor parameters match that wiring.  Override if your
// board has the RA8875 on different GPIOs.

#include "PicoSpiTransport.hpp"

namespace hipi {

PicoSpiTransport::PicoSpiTransport(spi_inst_t* spi_inst,
                                   std::uint32_t baudrate,
                                   std::uint32_t cs_gpio,
                                   std::uint32_t rst_gpio)
    : spi_(spi_inst),
      cs_(cs_gpio),
      rst_(rst_gpio) {
    // Bring up the SPI peripheral at the requested baudrate.  The caller
    // is responsible for configuring SCK/MOSI/MISO function-select via
    // gpio_set_function() — see examples/pico_main.cpp.
    // spi_init() returns the ACTUAL baudrate the peripheral could
    // achieve (the RP2350's clock divider can't hit every requested
    // value exactly) -- captured here so a caller can verify/log what
    // clock speed is really in effect, e.g. after raising the requested
    // value toward this chip's own documented 50MHz maximum (LT7683.pdf
    // Table A-21, "CLK SPI Input Clock ... Max. 50 MHz").
    actualBaudrate_ = spi_init(spi_, baudrate);

    // CS pin: drive high initially (inactive).
    gpio_init(cs_);
    gpio_set_dir(cs_, GPIO_OUT);
    gpio_put(cs_, 1);

    // RST pin: idle high (the controller is in reset while LOW).
    // Skipped entirely if not wired (NO_RESET_PIN) -- e.g. if this GPIO
    // is shared with something else (like an LED) that must own it instead.
    if (rst_ != NO_RESET_PIN) {
        gpio_init(rst_);
        gpio_set_dir(rst_, GPIO_OUT);
        gpio_put(rst_, 1);
    }
}

void PicoSpiTransport::csLow()  { gpio_put(cs_,  0); }
void PicoSpiTransport::csHigh() { gpio_put(cs_,  1); }
void PicoSpiTransport::rstLow()  { if (rst_ != NO_RESET_PIN) gpio_put(rst_, 0); }
void PicoSpiTransport::rstHigh() { if (rst_ != NO_RESET_PIN) gpio_put(rst_, 1); }

void PicoSpiTransport::spiTransfer(const std::uint8_t* tx,
                                   std::uint8_t*       rx,
                                   std::size_t         len) {
    if (len == 0) return;

    if (tx != nullptr && rx != nullptr) {
        // Full-duplex write/read of len bytes.
        spi_write_read_blocking(spi_, tx, rx, len);
    } else if (tx != nullptr) {
        spi_write_blocking(spi_, tx, len);
    } else if (rx != nullptr) {
        spi_read_blocking(spi_, 0, rx, len);
/*        // RA8875/LT7683 status / data reads: the controller wants a
        // dummy byte clocked out (the SPI bus is full-duplex, so
        // reading ANY byte back means clocking something out on MOSI
        // at the same time -- 0xFF is the pad value the MicroPython
        // reference code this was ported from used) for EVERY byte
        // being read, not just the first -- confirmed on real hardware
        // that multi-byte CGRAM read-back (see LT7683::readData(buf,
        // len)) only ever got its first byte correctly, with everything
        // after silently left untouched, because this used to always
        // call spi_write_read_blocking(...) with a hardcoded length of
        // 1 no matter how large `len` actually was. Chunked through a
        // small stack buffer (padChunk_) rather than a single `len`-
        // sized heap allocation -- len is always small in this project
        // (16 bytes for a CGRAM glyph, 1 for a status/data byte), so
        // this avoids ever needing to allocate for it, at the cost of
        // an extra loop iteration on the rare case len exceeds one
        // chunk's size.
        constexpr std::size_t kPadChunkSize = 32;
        static const std::uint8_t padChunk_[kPadChunkSize] = {
            0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
            0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
            0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
            0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        };
        std::size_t done = 0;
        while (done < len) {
            const std::size_t chunk = (len - done < kPadChunkSize) ? (len - done) : kPadChunkSize;
            spi_write_read_blocking(spi_, padChunk_, rx + done, chunk);
            done += chunk;
        }**/
    }
}

void PicoSpiTransport::delayMs(std::uint32_t ms) {
    sleep_ms(ms);
}

void PicoSpiTransport::delayUs(std::uint32_t us) {
    sleep_us(us);
}

}  // namespace hipi
