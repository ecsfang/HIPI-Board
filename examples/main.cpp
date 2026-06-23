// SPDX-License-Identifier: MIT
//
// Minimal HIPI demo on Linux over spidev.
//
// Build with CMake (see ../CMakeLists.txt).

#include "LinuxSpiDevTransport.hpp"
#include "RA8875.hpp"
#include "Screen.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

constexpr std::uint8_t FONT_COLOR    = 0xFF;  // foreground index in 8BPP mode
constexpr std::uint8_t BG_COLOR      = 0x00;
constexpr std::uint8_t TEXT_SIZE     = 0;     // 0..3 = built-in CGRAM modes
constexpr std::uint8_t BRIGHTNESS    = 200;

void exportGpio(int pin) {
    std::string path = "/sys/class/gpio/export";
    int fd = ::open(path.c_str(), O_WRONLY);
    if (fd >= 0) {
        const std::string s = std::to_string(pin);
        ::write(fd, s.c_str(), s.size());
        ::close(fd);
    }
    // Set direction to output
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
                     "Usage: %s <spidev-path> <cs-gpio#> <rst-gpio#>\n"
                     "  e.g. %s /dev/spidev0.0 25 24\n",
                     argv[0], argv[0]);
        return 1;
    }

    const std::string spi_path = argv[1];
    const int cs_pin  = std::atoi(argv[2]);
    const int rst_pin = std::atoi(argv[3]);

    exportGpio(cs_pin);
    exportGpio(rst_pin);

    int fd = ::open(spi_path.c_str(), O_RDWR);
    if (fd < 0) {
        std::perror("open spidev");
        return 1;
    }

    hipi::LinuxSpiDevTransport transport(fd, gpioValuePath(cs_pin),
                                            gpioValuePath(rst_pin), 6'000'000);

    hipi::RA8875 display(transport, /*width=*/800, /*height=*/480);
    display.begin();

    // Mirror share.py setup: 8BPP + 2-layer config.
    display.set8Bpp();
    display.set2LayerConfig();

    hipi::Screen screen(display, FONT_COLOR, TEXT_SIZE, BRIGHTNESS);

    // Simple demo: write a few lines, scroll, clear.
    const char* msg = "HELLO, WORLD!";
    for (const char* p = msg; *p; ++p) screen.pr_char(*p);
    screen.pr_char('\n');
    screen.pr_char('>');
    screen.pr_char(' ');
    screen.pr_char('4');
    screen.pr_char('2');
    screen.pr_char('\n');

    return 0;
}
