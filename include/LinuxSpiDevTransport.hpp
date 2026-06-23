// SPDX-License-Identifier: MIT
//
// Linux spidev transport for the RA8875 LCD driver.
//
// Wire up /dev/spidevX.Y, the CS GPIO, and the RST GPIO and pass this
// transport to RA8875().

#pragma once

#include "RA8875Transport.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

namespace hp82163 {

class LinuxSpiDevTransport : public RA8875Transport {
public:
    // Construct over an already-opened spidev file descriptor.
    //   spidev_fd      — open fd returned by ::open("/dev/spidevX.Y", O_RDWR)
    //   cs_gpio_path   — sysfs path for the chip-select GPIO, e.g.
    //                    "/sys/class/gpio/gpio25/value"
    //   rst_gpio_path  — sysfs path for the reset GPIO
    //   spi_speed_hz   — clock rate, e.g. 6'000'000
    LinuxSpiDevTransport(int         spidev_fd,
                         std::string cs_gpio_path,
                         std::string rst_gpio_path,
                         std::uint32_t spi_speed_hz = 6'000'000);

    ~LinuxSpiDevTransport() override;

    void csLow()  override;
    void csHigh() override;
    void rstLow()  override;
    void rstHigh() override;

    void spiTransfer(const std::uint8_t* tx,
                     std::uint8_t*       rx,
                     std::size_t         len) override;

    void delayMs(std::uint32_t ms) override;

private:
    void writeGpio(const std::string& path, int value);

    int         fd_;
    std::string cs_path_;
    std::string rst_path_;
};

}  // namespace hp82163
