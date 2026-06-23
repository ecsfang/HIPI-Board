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
    spi_init(spi_, baudrate);

    // CS pin: drive high initially (inactive).
    gpio_init(cs_);
    gpio_set_dir(cs_, GPIO_OUT);
    gpio_put(cs_, 1);

    // RST pin: idle high (the controller is in reset while LOW).
    gpio_init(rst_);
    gpio_set_dir(rst_, GPIO_OUT);
    gpio_put(rst_, 1);
}

void PicoSpiTransport::csLow()  { gpio_put(cs_,  0); }
void PicoSpiTransport::csHigh() { gpio_put(cs_,  1); }
void PicoSpiTransport::rstLow()  { gpio_put(rst_, 0); }
void PicoSpiTransport::rstHigh() { gpio_put(rst_, 1); }

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
        // RA8875 status / data reads: the controller wants the command
        // byte shifted out (DATRD/CMDRD), then the response byte clocked
        // back.  The MicroPython code does this with separate write+read
        // calls; emulate that with a dummy 0xFF byte.
        const std::uint8_t pad = 0xFF;
        spi_write_read_blocking(spi_, &pad, rx, 1);
    }
}

void PicoSpiTransport::delayMs(std::uint32_t ms) {
    sleep_ms(ms);
}

}  // namespace hipi
