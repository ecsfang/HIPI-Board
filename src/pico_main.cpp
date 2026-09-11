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
//   picotool load -f build/hipi_pico_demo.uf2
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
#include "display_config.h"
#include "Screen.hpp"
#include "touch.h"
#include "leds.h"
#ifdef TEST_DISPLAY
#include "display_boot_test.hpp"
#endif

// SD-card support
#include "ff.h"
#include "f_util.h"
#include "hw_config.h"
#include "hpil.h"
#include "display.h"  // videoDisplay -- see its own comment for why

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
#include "drive.h"

#include <cstdio>

// NeoPixel support
#include "../PicoLed/PicoLed.hpp"
#include "../PicoLed/Effects/Bounce.hpp"

hipi::Config config;

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
constexpr const char* HIPI_VERSION = "2.1(beta)";  // shown on splash screen
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

#define LED_PIN 26
#define LED_LENGTH 2
#define LED_PIO pio2

// NeoPixels test
void neoTest()
{
    // 0. Initialize LED strip
    LOGF("\n\r0. Initialize LED strip\n\r");
    auto ledStrip = PicoLed::addLeds<PicoLed::WS2812B>(LED_PIO, 0, LED_PIN, LED_LENGTH, PicoLed::FORMAT_GRB);
    //if( !ledStrip ) {
    //    LOGF("Failed to initialize LED strip on PIO %d, state machine %d, pin %d\n\r", LED_PIO, 0, LED_PIN);
    //    return;
    //}
    ledStrip.setBrightness(64);
    LOGF("1. Clear the strip!\n\r");
    
    // 1. Set all LEDs to red!
    LOGF("1. Set all LEDs to red!\n\r");
    ledStrip.fill( PicoLed::RGB(255, 0, 0) );
    ledStrip.show();
    sleep_ms(500);

    // 2. Set all LEDs to green!
    LOGF("2. Set all LEDs to green!\n\r");
    ledStrip.fill( PicoLed::RGB(0, 255, 0) );
    ledStrip.show();
    sleep_ms(500);

    // 3. Set all LEDs to blue!
    LOGF("3. Set all LEDs to blue!\n\r");
    ledStrip.fill( PicoLed::RGB(0, 0, 255) );
    ledStrip.show();
    sleep_ms(500);

    // 4. Set half LEDs to red and half to blue!
    LOGF("4. Set gradient from red to blue!\n\r");
    ledStrip.fillGradient( PicoLed::RGB(255, 0, 0), PicoLed::RGB(0, 0, 255) );
    ledStrip.show();
    sleep_ms(1000);

    // 5. Set half LEDs to red and half to blue!
    LOGF("5. Set rainbow colors!\n\r");
    ledStrip.fillRainbow(0, 255 / LED_LENGTH);
    ledStrip.show();
    sleep_ms(1000);
}

hipi::Screen *screen;
hipi::UiDialog *dialog = nullptr;

hipi::PicoSpiTransport* transport = nullptr;
hipi::DisplayDriver* display = nullptr;

FATFS fs;

void initDisplay()
{
    gpio_set_function(2, GPIO_FUNC_SPI);   // SCK
    gpio_set_function(3, GPIO_FUNC_SPI);   // MOSI
    gpio_set_function(0, GPIO_FUNC_SPI);   // MISO

    // Raised from the original 6MHz -- confirmed against LT7683.pdf's
    // own electrical characteristics table (Table A-21, "CLK SPI Input
    // Clock ... Max. 50 MHz") that this chip is rated far higher; 6MHz
    // was leaving most of the chip's own bandwidth unused, and was
    // confirmed to be the dominant cost in a full-screen bitmap redraw
    // (the button strip's own slide animation) on real hardware -- both
    // the BTE and non-BTE draw paths measured close to the theoretical
    // minimum SPI transfer time for the amount of pixel data involved,
    // regardless of which draw path or how efficiently it batched
    // writes. 30MHz leaves some margin below the chip's own 50MHz
    // maximum for real-world signal integrity (wire length, etc.) while
    // still being a large, direct multiple of the old speed.
    transport = new hipi::PicoSpiTransport(spi0,
                                              /*baudrate=*/30'000'000,
                                              /*cs_gpio=*/1,
                                              /*rst_gpio=*/hipi::PicoSpiTransport::NO_RESET_PIN);
    LOGF("\r\n * SPI baudrate: requested 30000000, actual %lu",
         static_cast<unsigned long>(transport->actualBaudrate()));
    display = new hipi::DisplayDriver(*transport, SCREEN_MAX_X, SCREEN_MAX_Y);

    display->begin();
}

FRESULT initSD()
{
    // Init SD-card ...
    LOGF("\r\n * Init SD-card ... ");
    init_spi();
    FRESULT fr = f_mount(&fs, "", 1);
    if (FR_OK != fr) {
        disableSD();
    } else {
        enableSD();
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

    //ledA.blink(100, 150);
    neoTest();

    initDisplay();

    FRESULT fr = initSD();

    // begin() already configures SYSR_16BPP as the default -- no override
    // needed. (Used to call display->set8Bpp() here to drop to 8bpp/RGB332;
    // switched to full 16bpp/RGB565 for better color depth, especially in
    // dark tones.)

    // Splash screen -- shown as early as possible, stays up for a couple
    // of seconds while the rest of the boot sequence (buttons, SD-card
    // driven config, HP-IL devices) continues below.
    hipi::showSplashScreen(display, HIPI_VERSION, 2000);

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

#ifdef TEST_DISPLAY
    // Must run *after* the USB CDC wait loops above -- LOGF() output
    // written before tud_cdc_n_connected(0) goes true is silently
    // dropped (nothing is listening on the port yet), so placing this
    // any earlier (e.g. right after initDisplay(), before initSD()) means
    // the test still runs and drives the display correctly, but every one
    // of its LOGF() calls vanishes -- no status register readout, no
    // "Filling screen: ..." lines, nothing. SD-card status being
    // independent of the display test itself (see initSD()'s own
    // non-fatal failure handling) is unaffected by this move -- SD just
    // now happens to run first in *sequence*, not as a dependency.
    // Comment out (or #undef TEST_DISPLAY above) once the display is
    // confirmed working.
    hipi::runDisplayBootTest(display);
#endif

    LOGF("\r\n * Init display ...");
    LOGF("\r\n\t* %s", DISPLAY_DEVICE);
    // Show buttons -- draws and caches the strip, and tells us how wide it
    // is so Screen's initial text width can be sized around it.
    const std::uint16_t buttonStripWidth = hipi::boardui_loadButtonStrip(display);
    LOGF("\r\n\t* Draw text ... ");
    display->setActiveWindow(0, 0, SCREEN_MAX_X-buttonStripWidth-1, SCREEN_MAX_Y-1);
    screen = new hipi::Screen(display, config.textColor(), 1, config.brightness(), SCREEN_MAX_X-buttonStripWidth );

    screen->pr_char(27);
    screen->pr_char('<'); // Cursor off
    screen->pr_str("# HIPI - HP-IL Pico Interface #");

    if( usb_connected )
        screen->pr_str("USB connected!");
    else
        screen->pr_str("Stand alone - no USB");
    {
        char buf[64];
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

    // 10s, not 5 -- gives enough time to actually read the device
    // self-check list (see hipi_test()) that ends up on screen just
    // before this; a touch breaks out of the wait early (see below).
    absolute_time_t infoTimeout = make_timeout_time_ms(10000);

    tud_cdc_n_write_flush(0);
    tud_task();

    dialog = new hipi::UiDialog(display, *screen);
    dialog->setCurrentFile(config.filename());  // see its own comment
    dialog->setColorChangedCallback([](std::uint16_t c) { config.setTextColor(c); });
    dialog->setTraceChangedCallback([](bool t, bool d) { config.setTraceMode(t, d); });
    dialog->setFontSizeChangedCallback([](std::uint8_t s) { config.setFontSize(s); });
    dialog->setBrightnessChangedCallback([](std::uint8_t b) { config.setBrightness(b); });
    dialog->setColumnsChangedCallback([](std::uint8_t c) { config.setColumns(c); });
    dialog->setDeviceToggledCallback([](const std::string& name, bool enabled) {
        config.setDeviceEnabled(name, enabled);
    });

    hipi::boardui_init(screen, dialog, HIPI_VERSION);

    // Init touch sensor ...
    LOGF("\r\n * Init touch sensor ...");
    // touch.cpp now has a real FT5316 backend for DISPLAY_7INCH (see its
    // own file header) -- was GSL1680-only before, which would have been
    // talking the wrong protocol to this board's actual touch chip.
    touchInit();
    touch_set_tap_callback(hipi::boardui_handleTap);
    touch_set_release_callback(hipi::boardui_handleRelease);
    touch_set_swipe_callback(hipi::boardui_handleSwipe);
    LOGF("\r\n\t* %s: Boot up completed!", gTouchDevice);

    tud_task();

    // Init HPIL scanner ...
    LOGF("\r\n * Init HPIL interface ...");

    // Init HPIL scanner ...
    HpIlLoop hpil(IN_M_PIN, IN_P_PIN, OUT_M_PIN, OUT_P_PIN);

    // Setup all devices in the HPIL loop (display, drive, LEDs, PILBox)
    hipi_init();

    // Wire up the plotter's live-draw callbacks now that display/screen/
    // plotter all exist (plotter is set inside hipi_init() above).
    hipi::plotterview_init(display, screen, plotter);

    LOGF("\r\n\t* HP-IL initialized");
    {
        LOGF("\r\n\t* Loop-back test");
        hipi_test(hpil);
    }

    // Done! Start the HPIL monoitoring ...
    LOGF("\r\n=============================");
    LOGF("\r\nUp and running ...\r\n");

    // Drain any stale/spurious touch state before waiting for a real
    // one below -- the FT5316 controller can report a false "touch
    // down" for a moment right after its own wake sequence (the WAKE_PIN
    // toggling in touchInit(), or general power-up electrical noise on
    // the panel), and touch_is_down() reads the controller live with no
    // caching, so a stale reading here would make the wait loop below
    // exit immediately, as if someone had already tapped the screen.
    // Confirmed as intermittent, not every boot -- exactly what a
    // power-up transient racing against this code's own timing would
    // look like. Several reads with a small settle delay between them,
    // discarding all of them, rather than just one -- a single extra
    // read could still land during the same transient if it's longer
    // than one iteration.
    for (int i = 0; i < 5; ++i) {
        touch_is_down();
        sleep_ms(20);
    }

    while (!time_reached(infoTimeout)) {
        tud_task();
        usb_serial_flush_boot_log();
        // A raw touch check (not the full touch_poll()/tap-callback
        // flow) -- lets someone skip the wait early without also
        // processing this same touch as a real tap, which would show
        // the button strip right as boot finishes.
        if (touch_is_down()) break;
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
        hipi::boardui_poll();        // auto-hide timers + status LED poll
        hipi::plotterview_poll();    // view-switch splash auto-dismiss timer

        tight_loop_contents();
    }

    LOGF("Done!\r\n");

}
