// SPDX-License-Identifier: MIT
//
// HP82163 demo on Raspberry Pico 2 (RP2350) using the pico-sdk.
//
// Pinout mirrors the MicroPython share.py wiring, except RST:
//   spi0  SCK=GP2  MOSI=GP3  MISO=GP0
//   CS          = GP1   (active-low)
//   RST         — not wired on this board (module has its own power-on
//                 reset); PicoSpiTransport::NO_RESET_PIN is passed instead
//                 of a GPIO, freeing GP4 for LED_PIN_1 (was a pin conflict:
//                 GP4 driven high to release RST was lighting LED_PIN_1 as
//                 a side effect before the splash screen even showed up)
//
// Build via the Raspberry Pi Pico VS Code extension, or:
//
//   cmake -DPICO_SDK_PATH=/path/to/pico-sdk -B build
//   cmake --build build
//   picotool load -f build/hp82163_pico_demo.uf2
//
// Touch handling (debounced tap/release state machine) lives in touch.h/
// touch.cpp. Splash screen, the auto-hiding button strip, the info box,
// and the USB/PILBOX status LEDs live in boardui.h/boardui.cpp. This file
// just wires everything together.

//#define TEST_DISPLAY

#include <stdlib.h>
#include <cstring>
#include <vector>
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

#include "bsp/board.h"   // for board_init()
#include "usb_serial.h"

extern void init_spi(void);

#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "pico/time.h"

#include "tusb.h"

#include "boardui.h"
#include "plotterview.h"
#include "config.hpp"

#include <cstdio>

extern bool SDOK;
extern void sd_dir();

hp82163::Config config;

// Run the loop as long as true ...
bool bRunning = true;

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
// FONT_COLOR/TEXT_SIZE/BRIGHTNESS used to be hardcoded here; the defaults
// now live in Config.hpp and are overridden by CONFIG.TXT on the SD card
// once one exists.
constexpr const char* HIPI_VERSION = "1.0";
}  // namespace

extern void hipi_init(void);
extern bool hipi_loop(HpIlLoop& loop);
extern bool hipi_test(HpIlLoop& loop);

bool usb_connected = false;

void SetPinDriveStrength(uint pin, uint mA) {
    // gpio_set_drive_strength() is the Pico SDK's API for this
    // The type is named 'gpio_drive_strength' (not _t)

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
hp82163::UiDialog *dialog = nullptr;

hp82163::PicoSpiTransport* transport = nullptr;
hp82163::RA8875* display = nullptr;

FATFS fs;

void initDisplay()
{
    gpio_set_function(2, GPIO_FUNC_SPI);   // SCK
    gpio_set_function(3, GPIO_FUNC_SPI);   // MOSI
    gpio_set_function(0, GPIO_FUNC_SPI);   // MISO

    transport = new hp82163::PicoSpiTransport(spi0,
                                              /*baudrate=*/6'000'000,
                                              /*cs_gpio=*/1,
                                              /*rst_gpio=*/hp82163::PicoSpiTransport::NO_RESET_PIN);
    display = new hp82163::RA8875(*transport, SCREEN_MAX_X, SCREEN_MAX_Y);

    display->begin();
}

FRESULT initSD()
{
    // Init SD-card ...
    LOGF("\r\n * Init SD-card ... ");
    init_spi();
    FRESULT fr = f_mount(&fs, "", 1);
    if (FR_OK != fr) {
        SDOK = false;
    } else {
        SDOK = true;
        config.load();
        bTrace = config.trace();
        bExtTrace = config.extTrace();
    }
    return fr;
}

int main() {
    board_init(); 
    tusb_init();

    // Init all leds ...
    //alienBegin();
    //alienStartup(2000);

    ledA.blink(100, 150);

    initDisplay();

    FRESULT fr = initSD();

    // begin() already configures SYSR_16BPP as the default -- no override
    // needed. (Used to call display->set8Bpp() here to drop to 8bpp/RGB332;
    // switched to full 16bpp/RGB565 for better color depth, especially in
    // dark tones.)

    // Splash screen -- shown as early as possible, stays up for a couple
    // of seconds while the rest of the boot sequence (buttons, SD-card
    // driven config, HP-IL devices) continues below.
    hp82163::showSplashScreen(display, HIPI_VERSION, 2000);

    absolute_time_t timeout = make_timeout_time_ms(2000);

    // Wait until USB CDC is connected (max 3 seconds)
    while (!time_reached(timeout)) {
        tud_task();  // drives the USB stack (host requests, enumeration, IN/OUT)
        if (tud_mounted()) {
            usb_connected = true;
            //ledPower.on();
            break;
        }
        sleep_ms(10);   // a bit gentler than tight_loop_contents
    }

    // Wait for a terminal to actually open the CDC port too (max 2 seconds --
    // previously unbounded, so boot would hang forever with no USB present).
    absolute_time_t cdcTimeout = make_timeout_time_ms(2000);
    while (!tud_cdc_n_connected(0) && !time_reached(cdcTimeout)) {
        tud_task();
        sleep_ms(10);
    }

    ledA.off();

    // PIL status LED is driven live by boardui_poll() (tud_cdc_n_connected(
    // ITF_HPIL)), no manual wiring needed here.

    // First here we know if the debug-port is open or not (usb_connected)
    LOGF("\r\n\nHIPI Board v%s\r\n", HIPI_VERSION);
    LOGF("======================");
    LOGF("\r\n * Init SD-card ... ");
    if (FR_OK != fr) {
        LOGF(" PANIC: f_mount error: %s (%d)\r\n", FRESULT_str(fr), fr);
    } else {
        LOGF("\r\n\t* SD-card mounted!");
    }

    tud_task();

    LOGF("\r\n * Init display ...");
    // Show buttons -- draws and caches the strip, and tells us how wide it
    // is so Screen's initial text width can be sized around it.
    const std::uint16_t buttonStripWidth = hp82163::boardui_loadButtonStrip(display);

    LOGF("\r\n\t* Draw text ... ");
    display->setActiveWindow(0, 0, SCREEN_MAX_X-buttonStripWidth-1, SCREEN_MAX_Y-1);
    screen = new hp82163::Screen(display, config.textColor(), 1, config.brightness(), SCREEN_MAX_X-buttonStripWidth );

    //screen->setTextSize(0);
    screen->pr_char(27);
    screen->pr_char('<'); // Cursor off
    screen->pr_str("###############################");
    screen->pr_str("# HIPI - HP-IL Pico Interface #");
    screen->pr_str("###############################");

    if( usb_connected )
        screen->pr_str("USB connected!");
    else
        screen->pr_str("Stand alone - no USB");
    {
        char buf[64];
        screen->pr_str("Config:");
        sprintf(buf, " * Drive: %.32s", config.filename().c_str() );
        screen->pr_str(buf);
        int z = sprintf(buf, " * Trace: " );
        switch( (config.trace() ? 0b10 : 0b00) | (config.extTrace() ? 0b01 : 0b00) ) {
        case 0b00: sprintf(buf+z, "OFF" ); break;
        case 0b10: sprintf(buf+z, "ON" ); break;
        case 0b11: sprintf(buf+z, "Extended" ); break;
        }
        screen->pr_str(buf);
        sprintf(buf, " * Font: %d", config.fontSize() );
        screen->pr_str(buf);
        sprintf(buf, " * Columns: %d", config.columns() );
        screen->pr_str(buf);
        screen->pr_str("--------------------------------------");
    }

    absolute_time_t infoTimeout = make_timeout_time_ms(5000);

    tud_cdc_n_write_flush(0);
    tud_task();

    dialog = new hp82163::UiDialog(display, *screen);
    dialog->setColorChangedCallback([](std::uint16_t c) { config.setTextColor(c); });
    dialog->setTraceChangedCallback([](bool t, bool d) { config.setTraceMode(t, d); });
    dialog->setFontSizeChangedCallback([](std::uint8_t s) { config.setFontSize(s); });
    dialog->setBrightnessChangedCallback([](std::uint8_t b) { config.setBrightness(b); });
    dialog->setColumnsChangedCallback([](std::uint8_t c) { config.setColumns(c); });
    dialog->setDeviceToggledCallback([](const std::string& name, bool enabled) {
        config.setDeviceEnabled(name, enabled);
    });

    hp82163::boardui_init(screen, dialog, HIPI_VERSION);

    // Init touch sensor ...
    LOGF("\r\n * Init touch sensor ...");
    touchInit();
    touch_set_tap_callback(hp82163::boardui_handleTap);
    touch_set_release_callback(hp82163::boardui_handleRelease);
    touch_set_swipe_callback(hp82163::boardui_handleSwipe);
    LOGF("\r\n\t* GSL1680 Boot up completed!");
    tud_task();

    // Init HPIL scanner ...
    LOGF("\r\n * Init HPIL interface ...");

    // Init HPIL scanner ...
    HpIlLoop hpil(IN_M_PIN, IN_P_PIN, OUT_M_PIN, OUT_P_PIN);

    // Setup all devices in the HPIL loop (display, drive, LEDs, PILBox)
    hipi_init();

    // Wire up the plotter's live-draw callbacks now that display/screen/
    // plotter all exist (plotter is set inside hipi_init() above).
    hp82163::plotterview_init(display, screen, plotter);

    LOGF("\r\n\t* HP-IL initialized");
    {
        LOGF("\r\n\t* Loop-back test");
        hipi_test(hpil);
    }

    // Done! Start the HPIL monoitoring ...
    LOGF("\r\n-----------------------------");
    LOGF("\r\nUp and running ...\r\n");

    while (!time_reached(infoTimeout)) {
        tud_task();
        usb_serial_flush_boot_log();
        sleep_ms(10);
    }

    screen->clear();
    screen->setTextSize(config.fontSize());
    if (config.columns() != 0) screen->setColumns(config.columns());

    while (bRunning) {
        tud_task();                     // TinyUSB background task
        usb_serial_flush_boot_log();    // flush buffered boot messages once a terminal connects
        hipi_loop(hpil);                // Check IL for any instruction to send through the loop!
        touch_poll();                   // debounced tap/release detection (touch.h)
        hp82163::boardui_poll();        // auto-hide timers + status LED poll
        hp82163::plotterview_poll();    // view-switch splash auto-dismiss timer
        tight_loop_contents();
    }

    LOGF("Done!\r\n");

}
