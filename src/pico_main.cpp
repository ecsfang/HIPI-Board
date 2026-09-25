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
// Separate from TEST_DISPLAY -- runs ONLY runFontTest() (see its own
// comment in display_boot_test.hpp), skipping touch calibration,
// primitives, splash screen etc. entirely, for fast iteration while
// this one specific question is still open. Safe to enable alongside
// TEST_DISPLAY too (runs first, see the call site below).
//#define TEST_FONT

#include <stdlib.h>
#include <cstring>
#include <vector>
#include "pico/bootrom.h"   // reset_usb_boot()
#include "pico/stdlib.h"
#include <stdio.h>
#include <pico/stdio.h>
#include <cstdint>

#include "hipi.h"

#include "PicoSpiTransport.hpp"
#include "display_config.h"
#include "Screen.hpp"
#include "touch.h"
#include "i2c_device.h"
#include "leds.h"
#if defined(TEST_DISPLAY) || defined(TEST_FONT)
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
constexpr const char* HIPI_VERSION = "2.5";  // shown on splash screen
}  // namespace

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

// NeoPixel support: see ilpixels.h/pixels.h -- the real CHipiPixel device
// (registered in hipi_init()) supersedes this file's own earlier
// neoTest() prototype (send/receive over HP-IL like every other device,
// rather than a fixed boot-time color-cycle demo run directly from here).

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
    // neoTest() removed -- superseded by the real CHipiPixel device (see
    // ilpixels.h/hipi_init()), which uses the very same PIXEL_PIN/PIXEL_PIO
    // via the global pixelStrip instance (pixels.cpp). That instance is
    // constructed before main() even runs (static init order), so calling
    // neoTest() here too would have doubly claimed the same PIO state
    // machine/pin.

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

#ifdef TEST_FONT
    // Same LOGF-timing reasoning as TEST_DISPLAY below -- must run after
    // the USB CDC wait loops above, or its output is silently dropped.
    hipi::runFontTest(display);
#endif

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
    LOGF("\r\n\t* SPI baudrate: requested 30000000, actual %lu",
         static_cast<unsigned long>(transport->actualBaudrate()));
#ifdef DISPLAY_7INCH
    // TEMPORARY diagnostic -- see LT7683::verifyCgramChar()'s own
    // comment (LT7683.hpp) for why the actual LOGF calls live HERE
    // rather than in a method on LT7683 itself: that class is part of
    // hipi_core_7 (see CMakeLists.txt), shared with the Linux demo
    // build, which never links tinyusb_device -- usb_serial.h's own
    // tusb.h include isn't reachable there at all. pico_main.cpp IS
    // part of the USB-linked executable target, so LOGF is safe to call
    // directly here instead. tusb_init() has already run by this point
    // (main() calls it before initSD(), which is what calls this
    // function), so LOGF() itself is safe to use; bTrace isn't set yet
    // (that happens later in THIS SAME function, from config.load()),
    // which is why this isn't just a TRC_LOGF call instead. Remove this
    // whole block once the underlying "custom font shows nothing" issue
    // (see Screen.cpp's own comment on the currently-reverted
    // selectCustomFont() call) is found and fixed.
    {
        struct Check { std::uint8_t code; const std::uint8_t* bitmap; const char* label; };
        uint8_t res[16];
        const Check checks[] = {
            { static_cast<std::uint8_t>('A'), hipi::font['A' - hipi::FONT_FIRST_ASCII], "'A'" },
            { static_cast<std::uint8_t>('0'), hipi::font['0' - hipi::FONT_FIRST_ASCII], "'0'" },
            { hipi::extra_font[0].code, hipi::extra_font[0].bitmap, "extra_font[0] ('\xC3\x84', Ä)" },
        };
        mLOGF("LT7683", "CGRAM upload verification (a few representative chars):");
        int i=0;
        for (const auto& c : checks) {
            const bool ok = display->verifyCgramChar(c.code, c.bitmap, res);
            LOGF("\r\n  code=0x%02X (%s): read-back %s {%02X}", c.code, c.label,
                 ok ? "MATCHES what was uploaded" : "DOES NOT MATCH -- upload/readback broken", res[i++]);
        }
    }
#else
    {
        uint32_t addr = 0x1234ABCD;
        uint8_t *pData = (uint8_t*)&addr;
        uint16_t data1 = addr & 0xFFFF;
        uint16_t data2 = (addr >> 16) & 0xFFFF;
        mLOGF("TEST", "%04X --> %02X %02X", data1, data1 & 0xFF, (data1 >> 8) & 0xFF);
        mLOGF("TEST", "%04X --> %02X %02X", data2, data2 & 0xFF, (data2 >> 8) & 0xFF);
        mLOGF("TEST", "%02X %02X %02X %02X", pData[0], pData[1], pData[2], pData[3]);
    }
#endif
    // Show buttons -- draws and caches the strip, and tells us how wide it
    // is so Screen's initial text width can be sized around it.
    const std::uint16_t buttonStripWidth = hipi::boardui_loadButtonStrip(display);
    LOGF("\r\n\t* Draw text ... ");
    display->setActiveWindow(0, 0, SCREEN_MAX_X-buttonStripWidth-1, SCREEN_MAX_Y-1);
    screen = new hipi::Screen(display, config.textColor(), 1, config.brightness(), SCREEN_MAX_X-buttonStripWidth );

    // Init touch sensor NOW, before any info text is drawn -- MOVED here
    // (was right before the wait-for-touch loop much further down, well
    // after the info text below had already been written) specifically
    // so the drain loop just below runs and finishes BEFORE the info
    // text appears, not after. On a true cold boot (as opposed to a
    // warm reset), the touch controller's own power rail and the I2C
    // bus can both still be settling right as this runs -- confirmed
    // that a stale/spurious "touch down" reading during that window
    // could otherwise still be sitting there by the time the
    // wait-for-touch loop (further down) starts checking touch_is_down(),
    // making it exit immediately as if someone had already tapped the
    // screen, even though the drain loop that already existed further
    // down (right before that wait loop) looked like it should have
    // caught this -- it ran too late, after the info text (and several
    // seconds of HPIL/device init) had already gone by, not before.
    LOGF("\r\n * Init touch sensor ...");
    // touch.cpp now has a real FT5316 backend for DISPLAY_7INCH (see its
    // own file header) -- was GSL1680-only before, which would have been
    // talking the wrong protocol to this board's actual touch chip.
    touchInit();
    // touchInit() just did the real i2c_init()/gpio_set_function() work
    // for this board's one physical I2C bus (see touch.h's own
    // touch_i2c) -- mark it so any CI2CDevice-derived device (e.g.
    // CHipiTemp's own CBmp280, set up later in hipi_init()) skips
    // repeating that when it comes up, rather than each device needing
    // to know touch got there first.
    CI2CBus::markInitialized(touch_i2c);

    // Drain any stale/spurious touch state before the info text below
    // is even drawn -- the FT5316 controller can report a false "touch
    // down" for a moment right after its own wake sequence (the
    // WAKE_PIN toggling in touchInit(), or general power-up electrical
    // noise on the panel), and touch_is_down() reads the controller
    // live with no caching, so a stale reading here would make the
    // wait-for-touch loop further down exit immediately, as if someone
    // had already tapped the screen. Confirmed as intermittent, not
    // every boot, and specifically a cold-boot symptom -- exactly what
    // a power-up transient racing against this code's own timing would
    // look like. Several reads with a small settle delay between them,
    // discarding all of them, rather than just one -- a single extra
    // read could still land during the same transient if it's longer
    // than one iteration.
    for (int i = 0; i < 5; ++i) {
        touch_is_down();
        sleep_ms(20);
    }

    screen->pr_char(27);
    screen->pr_char('<'); // Cursor off
    screen->pr_str("# HIPI - HP-IL Pico Interface #");

    if( usb_connected )
        screen->pr_str("USB connected!");
    else
        screen->pr_str("Stand alone - no USB");
    {
        char buf[64];
        sprintf(buf, " * Drive: %.32s",
                config.filename().empty() ? "No media" : config.filename().c_str() );
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

    // touchInit() and its drain loop already ran, much earlier -- see
    // the comment right after Screen construction above for why moving
    // them there was the actual fix. Just the callbacks left to wire up
    // here, now that boardui_init() above has run.
    touch_set_tap_callback(hipi::boardui_handleTap);
    touch_set_release_callback(hipi::boardui_handleRelease);
    touch_set_swipe_callback(hipi::boardui_handleSwipe);
    LOGF("\r\n\t* %s: Boot up completed!", gTouchDevice);

    tud_task();

    // Init HPIL scanner ...
    LOGF("\r\n * Init HPIL interface ...");

    // Init HPIL scanner ...
    HpIlLoop hpil(IN_M_PIN, IN_P_PIN, OUT_M_PIN, OUT_P_PIN);

    // Wired up here (rather than alongside the other dialog->set*Callback
    // calls above) since it needs hpil itself, which doesn't exist yet at
    // that point. Captured by reference -- hpil lives for main()'s own
    // (effectively unbounded, this never returns) lifetime, same as
    // every other object main() hands the dialog a callback into.
    dialog->setLoopbackTestCallback([&hpil](hipi::UiDialog::LoopbackProgressFn onProgress) {
        return hipi_loopbackTest(hpil, onProgress);
    });

    // Setup all devices in the HPIL loop (display, drive, LEDs, PILBox)
    hipi_init();

    // Wire up the plotter's live-draw callbacks now that display/screen/
    // plotter all exist (plotter is set inside hipi_init() above).
    hipi::plotterview_init(display, screen, plotter);
    hipi::plotterview_setTapeFile(config.filename());  // cassette in the Tape view

    LOGF("\r\n\t* HP-IL initialized");
    {
        LOGF("\r\n\t* Device self-check");
        hipi_test();
    }

    // Done! Start the HPIL monoitoring ...
    LOGF("\r\n=============================");
    LOGF("\r\nUp and running ...\r\n");

    // Touch queue was already drained right after Screen construction
    // above, before the info text was even written -- see that comment
    // for why doing it there (rather than just here, right before the
    // wait loop) is what actually fixes a cold-boot spurious touch.

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
