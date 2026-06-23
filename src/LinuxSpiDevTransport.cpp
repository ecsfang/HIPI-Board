// SPDX-License-Identifier: MIT
//
// Linux spidev transport — implementation.

#include "LinuxSpiDevTransport.hpp"

#include <fcntl.h>
#include <linux/spi/spidev.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <chrono>
#include <stdexcept>
#include <thread>

namespace hp82163 {

LinuxSpiDevTransport::LinuxSpiDevTransport(int         spidev_fd,
                                           std::string cs_gpio_path,
                                           std::string rst_gpio_path,
                                           std::uint32_t spi_speed_hz)
    : fd_(spidev_fd),
      cs_path_(std::move(cs_gpio_path)),
      rst_path_(std::move(rst_gpio_path)) {
    if (fd_ < 0) {
        throw std::runtime_error("LinuxSpiDevTransport: invalid spidev fd");
    }

    std::uint8_t mode = SPI_MODE_0;
    std::uint8_t lsb  = 0;
    if (ioctl(fd_, SPI_IOC_WR_MODE,            &mode)         < 0 ||
        ioctl(fd_, SPI_IOC_WR_LSB_FIRST,       &lsb)          < 0 ||
        ioctl(fd_, SPI_IOC_WR_MAX_SPEED_HZ,    &spi_speed_hz) < 0) {
        throw std::runtime_error("LinuxSpiDevTransport: ioctl setup failed");
    }
}

LinuxSpiDevTransport::~LinuxSpiDevTransport() = default;

void LinuxSpiDevTransport::writeGpio(const std::string& path, int value) {
    int fd = ::open(path.c_str(), O_WRONLY);
    if (fd < 0) {
        throw std::runtime_error("LinuxSpiDevTransport: cannot open " + path);
    }
    const char c = (value ? '1' : '0');
    if (::write(fd, &c, 1) != 1) {
        ::close(fd);
        throw std::runtime_error("LinuxSpiDevTransport: write to " + path + " failed");
    }
    ::close(fd);
}

void LinuxSpiDevTransport::csLow()  { writeGpio(cs_path_,  0); }
void LinuxSpiDevTransport::csHigh() { writeGpio(cs_path_,  1); }
void LinuxSpiDevTransport::rstLow()  { writeGpio(rst_path_, 0); }
void LinuxSpiDevTransport::rstHigh() { writeGpio(rst_path_, 1); }

void LinuxSpiDevTransport::spiTransfer(const std::uint8_t* tx,
                                       std::uint8_t*       rx,
                                       std::size_t         len) {
    if (len == 0) return;

    spi_ioc_transfer tr{};
    tr.tx_buf        = reinterpret_cast<std::uint64_t>(tx);
    tr.rx_buf        = reinterpret_cast<std::uint64_t>(rx);
    tr.len           = static_cast<__u32>(len);
    tr.speed_hz      = 0;       // keep configured max
    tr.delay_usecs   = 0;
    tr.bits_per_word = 8;
    tr.cs_change     = 0;       // we control CS via GPIO manually

    if (tx == nullptr && rx != nullptr) {
        // Half-duplex read — pass zeros for the command byte then read.
        const std::uint8_t zero[1] = { 0 };
        spi_ioc_transfer cmd{};
        cmd.tx_buf        = reinterpret_cast<std::uint64_t>(zero);
        cmd.rx_buf        = 0;
        cmd.len           = 1;
        cmd.bits_per_word = 8;
        if (ioctl(fd_, SPI_IOC_MESSAGE(1), &cmd) < 0) {
            throw std::runtime_error("LinuxSpiDevTransport: SPI_IOC_MESSAGE failed");
        }
    }

    if (ioctl(fd_, SPI_IOC_MESSAGE(1), &tr) < 0) {
        throw std::runtime_error("LinuxSpiDevTransport: SPI_IOC_MESSAGE failed");
    }
}

void LinuxSpiDevTransport::delayMs(std::uint32_t ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

}  // namespace hp82163
