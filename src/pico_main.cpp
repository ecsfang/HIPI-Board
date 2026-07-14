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

#include "bsp/board.h"   // för board_init()
#include "usb_serial.h"
#include <atomic>


extern void init_spi(void);

#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "pico/time.h"

#include "tusb.h"

#include "uidialog.hpp"
#include "config.hpp"
#include "bmp_loader.hpp"

#include <cstdio>

//#define PICO_DEFAULT_LED

//constexpr uint LED_PIN = 22;

extern bool SDOK;
extern void sd_dir();
extern bool bTrace;
extern bool bDebug;
hp82163::Config config;

bool drawBmpRightAligned(hp82163::RA8875& display, const char* path,
                         std::uint16_t screen_width, std::uint16_t y0,
                         std::vector<std::uint16_t>* outPixels = nullptr,
                         std::uint16_t* outWidth = nullptr,
                         std::uint16_t* outHeight = nullptr,
                         std::uint16_t* outScreenX0 = nullptr);
namespace {
bool touchActive = false;
absolute_time_t lastReleasePoll = get_absolute_time();
hp82163::Button pressedButton = hp82163::Button::None;

// Button-press visual feedback: redraw the button's bitmap shifted by
// (kPressDx, kPressDy) while held. kPressMargin (> the largest shift
// magnitude) is used for the baseline/restore redraws so any overflow
// outside the button's own rect -- in any direction -- gets cleaned up,
// not just the tight rect itself.
constexpr std::int16_t kPressDx = 5, kPressDy = -5;
constexpr std::int16_t kPressMargin = 8;
}  // namespace

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
constexpr const char* HIPI_VERSION = "1.0 Beta";
}  // namespace

extern void hipi_init(void);
extern bool hipi_loop(HpIlLoop& loop);

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
hp82163::UiDialog *dialog = nullptr;

// Cached copy of the button-strip bitmap, so a single button's
// sub-rectangle can be redrawn (e.g. shifted for "pressed" feedback)
// without re-reading the BMP from the SD card. Filled in by
// drawBmpRightAligned() at boot. RGB565 (2 bytes/pixel), matching the
// display's 16bpp mode.
std::vector<std::uint16_t> buttonStripPixels;
std::uint16_t buttonStripWidth = 0, buttonStripHeight = 0, buttonStripScreenX0 = 0;

extern void ledTest(void);
extern std::atomic<bool> g_dataReadyFlag;

int main() {
    //stdio_init_all();
    board_init(); 
    tusb_init();
    // Replace stdio_init_all() with:
    //usb_init();
    sleep_ms(500);

//    alienBegin();
//    alienStartup(2000);

    //ledTest();

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

    gpio_set_function(2, GPIO_FUNC_SPI);   // SCK
    gpio_set_function(3, GPIO_FUNC_SPI);   // MOSI
    gpio_set_function(0, GPIO_FUNC_SPI);   // MISO

    if( usb_connected ) {
        cdc0_printf("HIPI Board v0.1\r\n");
        cdc0_printf("======================");
        cdc0_printf("\r\n * Init display ...");
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
            //sd_dir();
        }
        SDOK = true;
        config.load();
        bTrace = config.trace();
        bDebug = config.debug();
    }
    
    tud_task();

    hp82163::PicoSpiTransport transport(spi0,
                                        /*baudrate=*/6'000'000,
                                        /*cs_gpio=*/1,
                                        /*rst_gpio=*/4);

    hp82163::RA8875 display(transport, /*width=*/800, /*height=*/480);

    display.begin();

    // begin() already configures SYSR_16BPP as the default -- no override
    // needed. (Used to call display.set8Bpp() here to drop to 8bpp/RGB332;
    // switched to full 16bpp/RGB565 for better color depth, especially in
    // dark tones.)

    // Splash screen -- shown as early as possible, stays up for a couple
    // of seconds while the rest of the boot sequence (buttons, SD-card
    // driven config, HP-IL devices) continues below.
    hp82163::showSplashScreen(display, HIPI_VERSION, 3000);

    // Show buttons ...
    cdc0_printf("\r\n * Draw buttons ... ");
    if (!drawBmpRightAligned(display, "buttons.bmp", 800, 0,
                             &buttonStripPixels, &buttonStripWidth,
                             &buttonStripHeight, &buttonStripScreenX0)) {
        cdc0_printf("\r\n ### Failed to draw buttons ... ");
    }

    display.setActiveWindow(0, 0, 679, 479);
    screen = new hp82163::Screen(display, config.textColor(), config.fontSize(), config.brightness(), 680 );
    if (config.columns() != 0) screen->setColumns(config.columns());

/**
    cdc0_printf("\r\n * Draw text ... ");
    const char* lines[] = {
        "HELLO, WORLD!",
        "HP82163 EMULATOR",
        "PICO 2 + RA8875"
    };
    for (const char* line : lines) {
        for (const char* p = line; *p; ++p) screen->pr_char(*p);
        screen->pr_char('\r');
        screen->pr_char('\n');
    }    

    tud_cdc_n_write_flush(0);
    tud_task();
***/
    dialog = new hp82163::UiDialog(display, *screen);
    dialog->setColorChangedCallback([](std::uint16_t c) { config.setTextColor(c); });
    dialog->setTraceChangedCallback([](bool t, bool d) { config.setTraceMode(t, d); });
    dialog->setFontSizeChangedCallback([](std::uint8_t s) { config.setFontSize(s); });
    dialog->setBrightnessChangedCallback([](std::uint8_t b) { config.setBrightness(b); });
    dialog->setColumnsChangedCallback([](std::uint8_t c) { config.setColumns(c); });

    // Init touch sensor ...
    if( usb_connected ) {
        cdc0_printf("\r\n * Init touch sensor ...");
        touchInit();
        cdc0_printf("\t* GSL1680 Boot up completed!\r\n");
    }
    tud_task();

    // Init HPIL scanner ...
    if( usb_connected ) {
        cdc0_printf("\r\n * Init HPIL interface ...");
    }

    // Init HPIL scanner ...
    HpIlLoop hpil(IN_M_PIN, IN_P_PIN, OUT_M_PIN, OUT_P_PIN);

    // Setup all devices in the HPIL loop (display, drive, LEDs, PILBox)
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
        // Snabb reaktion vid NYTT tryck (interrupt-flaggan satts av IRQ_PIN)
        if (g_dataReadyFlag.exchange(false, std::memory_order_relaxed)) {
            if (!touchActive) {
                std::uint16_t tx, ty;
                if (touch_get_point(tx, ty)) {
                    touchActive = true;
                    hp82163::Button b = hp82163::hitTestButton(tx, ty);
                    if (b != hp82163::Button::None) {
                        pressedButton = b;
                        // Visual feedback: redraw just this button's bitmap
                        // region shifted, so it looks pressed in. The
                        // baseline/restore redraws use a margin larger than
                        // the shift, sourced from the same strip cache, so
                        // any overflow outside the button's own rect (e.g.
                        // a shift toward the top-right sticks out past the
                        // rect's top and right edges) gets cleaned up too --
                        // not just the tight rect itself.
                        hp82163::redrawButtonRegion(
                            display, buttonStripPixels.data(),
                            buttonStripWidth, buttonStripHeight,
                            buttonStripScreenX0, /*stripScreenY0=*/0, b, 0, 0,
                            kPressMargin);
                        hp82163::redrawButtonRegion(
                            display, buttonStripPixels.data(),
                            buttonStripWidth, buttonStripHeight,
                            buttonStripScreenX0, /*stripScreenY0=*/0, b,
                            kPressDx, kPressDy);
                        dialog->handleButton(b);
                    }
                }
            }
            // Om touchActive redan ar true: ignorera — fingret ligger kvar,
            // vantar bara pa att lyftas innan nasta tryck raknas.
        }

        // Lattviktig poll (var ~30 ms) for att upptacka NAR fingret lyfts —
        // inget IRQ kommer da, sa vi maste fraga aktivt.
        if (touchActive &&
            absolute_time_diff_us(lastReleasePoll, get_absolute_time()) > 30'000) {
            lastReleasePoll = get_absolute_time();
            if (!touch_is_down()) {
                touchActive = false;
                if (pressedButton != hp82163::Button::None) {
                    // Restore the button's bitmap to its normal position
                    // (with the same margin, to clean up any overflow from
                    // the shift regardless of direction).
                    hp82163::redrawButtonRegion(
                        display, buttonStripPixels.data(),
                        buttonStripWidth, buttonStripHeight,
                        buttonStripScreenX0, /*stripScreenY0=*/0, pressedButton, 0, 0,
                        kPressMargin);
                    pressedButton = hp82163::Button::None;
                }
            }
       }
//        if (g_dataReadyFlag.exchange(false, std::memory_order_relaxed)) {
//            touch_read();
//        }
        tight_loop_contents();
    }

    if( usb_connected ) {
        cdc0_printf("Done!\r\n");
    }

}



bool drawBmpRightAligned(hp82163::RA8875& display, const char* path,
                         std::uint16_t screen_width, std::uint16_t y0,
                         std::vector<std::uint16_t>* outPixels,
                         std::uint16_t* outWidth,
                         std::uint16_t* outHeight,
                         std::uint16_t* outScreenX0) {
    std::uint16_t width = 0, height = 0;
    if (!hp82163::peekBmpDimensions(path, width, height)) {
        cdc0_printf("\r\n\t * Could not read BMP dimensions for <%s>!", path);
        return false;
    }
    const std::uint16_t x0 = static_cast<std::uint16_t>(screen_width - width);  // höger kant
    if (outScreenX0) *outScreenX0 = x0;
    return hp82163::drawBmpAt(display, path, static_cast<std::int16_t>(x0),
                              static_cast<std::int16_t>(y0),
                              outPixels, outWidth, outHeight);
}
