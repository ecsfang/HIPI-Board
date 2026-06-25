// SPDX-License-Identifier: MIT
//
// HP82163 demo on Raspberry Pico 2 (RP2350) using the pico-sdk.
//
// Pinout mirrors the MicroPython share.py wiring:
//   spi0  SCK=GP2  MOSI=GP3  MISO=GP0
//   CS          = GP1   (active-low)
//   RST         = GP4   (active-low)
//
// Build via the Raspberry Pi Pico VS Code extension, or:
//
//   cmake -DPICO_SDK_PATH=/path/to/pico-sdk -B build
//   cmake --build build
//   picotool load -f build/hp82163_pico_demo.uf2

#include <stdlib.h>

#include "hpil_pio.hpp"

#include "PicoSpiTransport.hpp"
#include "RA8875.hpp"
#include "Screen.hpp"

// SD-card support
#include "ff.h"
#include "f_util.h"
#include "hw_config.h"
//#include "pico/rtc.h"
extern void init_spi(void);

#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "pico/stdlib.h"
#include "pico/time.h"

#include <cstdio>

#define PICO_DEFAULT_LED

#ifdef PICO_DEFAULT_LED
    // Pico 2 W — använd CYW43-HAL:en
    #include "pico/cyw43_arch.h"
    #define BLINK_LED_TYPE_WIFI
#else
    // Pico 2 (utan W) — använd den inbyggda GPIO-LED:en på GP25
    constexpr uint LED_PIN = 25;
    #define BLINK_LED_TYPE_GPIO
#endif

namespace {
constexpr std::uint8_t FONT_COLOR  = 0xFF;  // foreground index in 8BPP mode
constexpr std::uint8_t TEXT_SIZE   = 0;     // 0..3 = built-in CGRAM modes
constexpr std::uint8_t BRIGHTNESS  = 200;
}  // namespace

extern void hipi_tests(void);

void led_on() {
#ifdef BLINK_LED_TYPE_WIFI
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);
#else
        gpio_put(LED_PIN, 1);
#endif
}
void led_off() {
#ifdef BLINK_LED_TYPE_WIFI
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);
#else
        gpio_put(LED_PIN, 0);
#endif
}

void blink_led(int t=250, int n=1) {
    for( int i=0; i<n; ++i) {
        // LED PÅ
        led_on();
        sleep_ms(t);

        // LED AV
        led_off();
        sleep_ms(t);
    }
}

static const uint16_t pattern[] = {
    60,  80,   // blink
    60, 300,   // blink
    120, 70,   // blink
    40,  900,  // lång paus
    50,  50,   // dubbelblink
    50,  500
};

static uint8_t state = 0;
static uint32_t next_time = 0;
static bool led_state = false;

void led_task(uint32_t now_ms)
{
    if (now_ms < next_time)
        return;

    led_state = !led_state;

    if (led_state)
        led_on();
    else
        led_off();

    next_time = now_ms + pattern[state] + (rand() % 40);

    state++;
    if (state >= sizeof(pattern)/sizeof(pattern[0]))
        state = 0;
}

static bool usb_connected = false;

int main() {
    stdio_init_all();

#ifdef BLINK_LED_TYPE_WIFI
    if (cyw43_arch_init() != 0) {
        // Init misslyckades — blinka med en utprintad felkod istället
        while (true) {
            printf("CYW43 init failed\n");
            sleep_ms(1000);
        }
    }
#else
    gpio_set_dir(LED_PIN, GPIO_OUT);
#endif

    // Blink LED:en 5 ggr Snabbt = "boot OK"
    blink_led(50, 10);


    absolute_time_t timeout = make_timeout_time_ms(1000);

    // Vänta tills USB-CDC är ansluten (max 3 sekunder)
    while (!time_reached(timeout)) {
        if (stdio_usb_connected()) {
            usb_connected = true;
            break;
        }

        blink_led(250);
        tight_loop_contents();
    }

    // SPI0: SCK=GP2, MOSI=GP3, MISO=GP0 (matches share.py)
    gpio_set_function(2, GPIO_FUNC_SPI);   // SCK
    gpio_set_function(3, GPIO_FUNC_SPI);   // MOSI
    gpio_set_function(0, GPIO_FUNC_SPI);   // MISO

    // CS=GP1 and RST=GP4 are configured by PicoSpiTransport's constructor.
    if( usb_connected ) {
        printf("HIPI Board v0.1\n");
        printf("======================");
        printf("\n * Init display ...");
    }

    hp82163::PicoSpiTransport transport(spi0,
                                        /*baudrate=*/6'000'000,
                                        /*cs_gpio=*/1,
                                        /*rst_gpio=*/4);

    hp82163::RA8875 display(transport, /*width=*/800, /*height=*/480);
    display.begin();

    // Mirror share.py post-init register writes:
    //   display._write_reg(display.SYSR, display.SYSR_8BPP)
    //   display._write_reg(0x20, 0x80)
    display.set8Bpp();
    display.set2LayerConfig();

    hp82163::Screen screen(display, FONT_COLOR, TEXT_SIZE, BRIGHTNESS);

    const char* lines[] = {
        "HELLO, WORLD!",
        "HP82163 EMULATOR",
        "PICO 2 + RA8875",
    };
    for (const char* line : lines) {
        for (const char* p = line; *p; ++p) screen.pr_char(*p);
        screen.pr_char('\n');
    }

    // Init SD-card ...
    if( usb_connected ) {
        printf("\n * Init SD-card ...");
    }
    init_spi();
    FATFS fs;
    FRESULT fr = f_mount(&fs, "", 1);
    if (FR_OK != fr) {
        printf(" PANIC: f_mount error: %s (%d)\n", FRESULT_str(fr), fr);
    }

    // Init touch sensor ...
    if( usb_connected ) {
        printf("\n * Init touch sensor ...");
        printf(" TBD!");
    }

    // Done! Start the HPIL monoitoring ...
    if( usb_connected ) {
        printf("\n-----------------------------");
        printf("\nUp and running ...\n");
        printf("Run HPIL tests ...\n");
    }

    hipi_tests();

    if( usb_connected ) {
        printf("Done!\n");
    }

    while (true) {
        led_task(to_ms_since_boot(get_absolute_time()));
        tight_loop_contents();
    }
}
