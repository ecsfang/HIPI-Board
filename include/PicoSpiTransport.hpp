// SPDX-License-Identifier: MIT
//
// Raspberry Pico 2 (RP2350) SPI transport for the RA8875 LCD driver.
//
// Uses the pico-sdk SPI master + GPIO helpers.  Build with:
//
//   add_library(hp82163_pico STATIC src/PicoSpiTransport.cpp)
//   target_link_libraries(hp82163_pico pico_stdlib hardware_spi hardware_gpio)
//
// in your CMakeLists.txt.

#pragma once

#include "RA8875Transport.hpp"

#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "pico/time.h"

#include <cstdint>

namespace hp82163 {

class PicoSpiTransport : public RA8875Transport {
public:
    // Pass NO_RESET_PIN for rst_gpio if the RA8875 module's RST line isn't
    // actually wired to the Pico (common -- many breakout boards handle
    // their own power-on reset). rstLow()/rstHigh() become no-ops in that
    // case, so the pin is never touched at all -- freeing it up entirely
    // for other uses (e.g. an LED) instead of just relocating the conflict.
    static constexpr std::uint32_t NO_RESET_PIN = 0xFFFFFFFFu;

    //   spi_inst   — spi0 or spi1
    //   baudrate   — e.g. 6'000'000  (the MicroPython code used 6 MHz on spi0)
    //   cs_gpio    — chip-select GPIO number
    //   rst_gpio   — reset GPIO number, or NO_RESET_PIN if not wired
    PicoSpiTransport(spi_inst_t* spi_inst,
                     std::uint32_t baudrate,
                     std::uint32_t cs_gpio,
                     std::uint32_t rst_gpio);

    void csLow()  override;
    void csHigh() override;
    void rstLow()  override;
    void rstHigh() override;

    void spiTransfer(const std::uint8_t* tx,
                     std::uint8_t*       rx,
                     std::size_t         len) override;

    void delayMs(std::uint32_t ms) override;

private:
    spi_inst_t* spi_;
    std::uint32_t cs_;
    std::uint32_t rst_;
};

}  // namespace hp82163
