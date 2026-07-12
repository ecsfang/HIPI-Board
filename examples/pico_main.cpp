// SPDX-License-Identifier: MIT
//
// HIPI demo on Raspberry Pico 2 (RP2350) using the pico-sdk.
//
// Pinout mirrors the MicroPython share.py wiring:
//   spi0  SCK=GP2  MOSI=GP3  MISO=GP0
//   CS          = GP1   (active-low)
//   RST         = GP4   (active-low)
//   SD detect   = GP7   (input, pull-up)   — only used in the SD code path
//
// spi1 SCK=GP10 MOSI=GP11 MISO=GP8          — for the optional SD-card path
//
// Build (pico-sdk project):
//
//   add_executable(hipi_pico_demo examples/pico_main.cpp src/PicoSpiTransport.cpp)
//   target_link_libraries(hipi_pico_demo
//       pico_stdlib hardware_spi hardware_gpio
//       hipi_core)
//   pico_enable_stdio_usb(hipi_pico_demo 1)
//   pico_enable_stdio_uart(hipi_pico_demo 0)

#include "PicoSpiTransport.hpp"
#include "RA8875.hpp"
#include "Screen.hpp"

#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "pico/stdlib.h"

#include <cstdio>

namespace {
constexpr std::uint8_t FONT_COLOR  = 0xFFFF;  // foreground index in 8BPP mode
constexpr std::uint8_t BG_COLOR    = 0x00;
constexpr std::uint8_t TEXT_SIZE   = 2;     // 0..3 = built-in CGRAM modes
constexpr std::uint8_t BRIGHTNESS  = 200;
}  // namespace

int main() {
    stdio_init_all();

    // SPI0: SCK=GP2, MOSI=GP3, MISO=GP0  (matches share.py)
    gpio_set_function(2, GPIO_FUNC_SPI);   // SCK
    gpio_set_function(3, GPIO_FUNC_SPI);   // MOSI
    gpio_set_function(0, GPIO_FUNC_SPI);   // MISO

    // CS=GP1 and RST=GP4 are configured by PicoSpiTransport's constructor.

    hipi::PicoSpiTransport transport(spi0,
                                        /*baudrate=*/6'000'000,
                                        /*cs_gpio=*/1,
                                        /*rst_gpio=*/4);

    hipi::RA8875 display(transport, /*width=*/800, /*height=*/480);
    display.begin();

    // Mirror share.py post-init register writes:
    //   display._write_reg(display.SYSR, display.SYSR_8BPP)
    //   display._write_reg(0x20, 0x80)
    display.set8Bpp();
    display.set2LayerConfig();

    hipi::Screen screen(display, FONT_COLOR, TEXT_SIZE, BRIGHTNESS);

    // Demo: print a few lines, scroll, then idle forever.
    const char* lines[] = {
        "HELLO, WORLD!",
        "HIPI EMULATOR",
        "PICO 2 + RA8875",
    };
    for (const char* line : lines) {
        for (const char* p = line; *p; ++p) screen.pr_char(*p);
        screen.pr_char('\n');
    }

    while (true) {
        tight_loop_contents();
    }
}
