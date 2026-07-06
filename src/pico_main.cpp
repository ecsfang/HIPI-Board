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

//#define TEST_DISPLAY

#include <stdlib.h>
#include "pico/bootrom.h"   // reset_usb_boot()
#include "pico/stdlib.h"
#include <stdio.h>
#include <pico/stdio.h>
#include <cstdint>

#include "hpil_pio.hpp"

#include "PicoSpiTransport.hpp"
#include "RA8875.hpp"
#include "Screen.hpp"
#include "touch.h"
#include "leds.h"
#ifdef TEST_DISPLAY
#include "display_test.hpp"
#endif

// SD-card support
#include "ff.h"
#include "f_util.h"
#include "hw_config.h"
#include "hpil.h"

#include "bsp/board.h"   // för board_init()
#include "usb_serial.h"


extern void init_spi(void);

#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "pico/time.h"

#include "tusb.h"

#include <cstdio>

//#define PICO_DEFAULT_LED

//constexpr uint LED_PIN = 22;

extern bool SDOK;
extern void sd_dir();


extern "C" bool tud_vendor_control_xfer_cb(uint8_t rhport,
                                            uint8_t stage,
                                            tusb_control_request_t const* request) {
    if (stage != CONTROL_STAGE_SETUP) return true;
    if (request->bmRequestType == 0x40 && request->bRequest == 0x01) {
        reset_usb_boot(0, 0);
    }
    return true;
}

/*
 * alien_startup.cpp
 *
 * Drives 5 LEDs through a dramatic "alien intelligence" boot sequence.
 *
 * Usage example (Arduino-Pico / Arduino IDE):
 * -----------------------------------------------
 *   #include "alien_startup.cpp"   // or add to your sketch directly
 *
 *   void setup() {
 *       alienBegin();          // configure all LED pins as OUTPUT
 *       alienStartup(2000);    // run the boot sequence for ~2 s
 *   }
 *
 *   void loop() {
 *       // your main code here
 *   }
 *
 * Usage example (Pico SDK):
 * -----------------------------------------------
 *   int main() {
 *       stdio_init_all();
 *       alienBegin();
 *       alienStartup(2000);
 *       while (true) { tight_loop_contents(); }
 *   }
 *
 * Wiring: connect LEDs (+ series resistor ~220 Ω) from each pin to GND.
 */

// ─── Compatibility shim ───────────────────────────────────────────────────────
#include "pico/stdlib.h"



namespace {
constexpr std::uint8_t FONT_COLOR  = 0xFF;  // foreground index in 8BPP mode
constexpr std::uint8_t TEXT_SIZE   = 0;     // 0..3 = built-in CGRAM modes
constexpr std::uint8_t BRIGHTNESS  = 200;
}  // namespace

extern void hipi_tests(void);
extern void hipi_init(void);
extern IL_CMD_t hipi_loop(HpIlLoop& loop);


static bool usb_connected = false;

void SetPinDriveStrength(uint pin, uint mA) {
    // gpio_set_drive_strength() är pico-SDK:s API för detta
    // Värdetypen heter 'gpio_drive_strength' (inte _t)
    
    if (mA <= 2) {
        gpio_set_drive_strength(pin, GPIO_DRIVE_STRENGTH_2MA);
    } else if (mA <= 4) {
        gpio_set_drive_strength(pin, GPIO_DRIVE_STRENGTH_4MA);
    } else if (mA <= 8) {
        gpio_set_drive_strength(pin, GPIO_DRIVE_STRENGTH_8MA);
    } else {
        gpio_set_drive_strength(pin, GPIO_DRIVE_STRENGTH_12MA);
    }
}

hp82163::Screen *screen;

extern void ledTest(void);

int main() {
    //stdio_init_all();
    board_init(); 
    tusb_init();
    // Replace stdio_init_all() with:
    //usb_init();
    sleep_ms(500);

//    alienBegin();
//    alienStartup(2000);

    ledTest();

    absolute_time_t timeout = make_timeout_time_ms(3000);

    // Vänta tills USB-CDC är ansluten (max 3 sekunder)
    while (!time_reached(timeout)) {
        tud_task();  // driver USB-stacken (host requests, enumeration, IN/OUT)
        if (tud_mounted()) {
            usb_connected = true;
            blink_led(LED_PIN_3, 250);
            break;
        }
        sleep_ms(10);   // lite snällare än tight_loop_contents
    }

    while(tud_cdc_n_connected(0) == false) {
        tud_task();
        sleep_ms(10);
    }
    blink_led(LED_PIN_1, 250);

    // SPI0: SCK=GP2, MOSI=GP3, MISO=GP0 (matches share.py)
    gpio_set_function(2, GPIO_FUNC_SPI);   // SCK
    gpio_set_function(3, GPIO_FUNC_SPI);   // MOSI
    gpio_set_function(0, GPIO_FUNC_SPI);   // MISO

//    breathing_led::init(LED_PIN_3); 

    // CS=GP1 and RST=GP4 are configured by PicoSpiTransport's constructor.
    if( usb_connected ) {
        cdc0_printf("HIPI Board v0.1\r\n");
        cdc0_printf("======================");
        cdc0_printf("\r\n * Init display ...");
    }

    // Interface 1 — new second port
    const char* msg = "Hello from CDC1 (/dev/ttyACM1)\r\n";
    cdc1_write(msg, strlen(msg));

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

    screen = new hp82163::Screen(display, FONT_COLOR, TEXT_SIZE, BRIGHTNESS);

    const char* lines[] = {
        "HELLO, WORLD!",
        "HP82163 EMULATOR",
        "PICO 2 + RA8875",
    };
    for (const char* line : lines) {
        for (const char* p = line; *p; ++p) screen->pr_char(*p);
        screen->pr_char('\r');
        screen->pr_char('\n');
    }

    // Init SD-card ...
    if( usb_connected ) {
        cdc0_printf("\r\n * Init SD-card ... ");
    }
    init_spi();
    FATFS fs;
    FRESULT fr = f_mount(&fs, "", 1);
    if (FR_OK != fr) {
        cdc0_printf(" PANIC: f_mount error: %s (%d)\r\n", FRESULT_str(fr), fr);
        SDOK = false;
    } else {
        if( usb_connected ) {
            cdc0_printf(" SD-card mounted!");
            sd_dir();
        }
        SDOK = true;
    }

    // Init touch sensor ...
    if( usb_connected ) {
        cdc0_printf("\r\n * Init touch sensor ...");
        touchTest();
    }

    // Init HPIL scanner ...
    if( usb_connected ) {
        cdc0_printf("\r\n * Init HPIL interface ...");
    }

    HpIlLoop hpil(IN_M_PIN, IN_P_PIN, OUT_M_PIN, OUT_P_PIN);

    hipi_init();

    if( usb_connected ) {
        cdc0_printf(" HP-IL initialized");
    }
    // Done! Start the HPIL monoitoring ...
    if( usb_connected ) {
        cdc0_printf("\r\n-----------------------------");
        cdc0_printf("\r\nUp and running ...\r\n");
    }

    bool bRunning = true;

    while (bRunning) {
        tud_task();  // TinyUSB background task
        hipi_loop(hpil);
        tight_loop_contents();
    }

    if( usb_connected ) {
        cdc0_printf("Done!\r\n");
    }

}
