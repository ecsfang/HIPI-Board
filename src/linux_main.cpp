// SPDX-License-Identifier: MIT
//
// Minimal HP82163 demo on Linux over /dev/spidevX.Y.
//
// Build (only when HP82163_BUILD_LINUX_EXAMPLE is ON):
//
//   cmake -DHP82163_BUILD_LINUX_EXAMPLE=ON -B build
//   cmake --build build
//   sudo ./build/hp82163_linux_demo /dev/spidev0.0 25 24

#include "LinuxSpiDevTransport.hpp"
#include "RA8875.hpp"
#include "Screen.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

constexpr std::uint16_t FONT_COLOR = 0xFFFF;  // RGB565 white
constexpr std::uint8_t TEXT_SIZE   = 0;
constexpr std::uint8_t BRIGHTNESS  = 200;

void exportGpio(int pin) {
    int fd = ::open("/sys/class/gpio/export", O_WRONLY);
    if (fd >= 0) {
        const std::string s = std::to_string(pin);
        ::write(fd, s.c_str(), s.size());
        ::close(fd);
    }
    std::string dir = "/sys/class/gpio/gpio" + std::to_string(pin) + "/direction";
    fd = ::open(dir.c_str(), O_WRONLY);
    if (fd >= 0) {
        const char out[] = "out";
        ::write(fd, out, sizeof(out) - 1);
        ::close(fd);
    }
}

std::string gpioValuePath(int pin) {
    return "/sys/class/gpio/gpio" + std::to_string(pin) + "/value";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr,
                     "Usage: %s <spidev-path> <cs-gpio#> <rst-gpio#>\n",
                     argv[0]);
        return 1;
    }

    const std::string spi_path = argv[1];
    const int cs_pin  = std::atoi(argv[2]);
    const int rst_pin = std::atoi(argv[3]);

    exportGpio(cs_pin);
    exportGpio(rst_pin);

    int fd = ::open(spi_path.c_str(), O_RDWR);
    if (fd < 0) { std::perror("open spidev"); return 1; }

    hp82163::LinuxSpiDevTransport transport(fd, gpioValuePath(cs_pin),
                                            gpioValuePath(rst_pin), 6'000'000);

    hp82163::RA8875 display(transport, 800, 480);
    display.begin();   // begin() already configures 16bpp by default
    display.set2LayerConfig();

    hp82163::Screen screen(&display, FONT_COLOR, TEXT_SIZE, BRIGHTNESS);

    const char* msg = "HELLO, WORLD!\n> 42\n";
    for (const char* p = msg; *p; ++p) screen.pr_char(*p);

    ::close(fd);
    return 0;
}
