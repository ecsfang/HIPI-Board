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

extern bool SDOK;
extern void sd_dir();
extern bool bTrace;
extern bool bDebug;
extern uint8_t hpilDevices;

hp82163::Config config;

bool drawBmpRightAligned(hp82163::RA8875* display, const char* path,
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
constexpr const char* HIPI_VERSION = "1.0";
}  // namespace

extern void hipi_init(void);
extern bool hipi_loop(HpIlLoop& loop);

bool usb_connected = false;

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
        bDebug = config.debug();
    }
    return fr;
}

// ── Auto-hiding button strip ────────────────────────────────────────────────
//
// Normally hidden, so the text area gets the full panel width. Any touch
// anywhere on the screen -- whether that's a tap squarely on a button or
// just the start of a drag across the display -- reveals the strip and
// resets the countdown; hitTestButton() works purely on fixed screen
// coordinates, so it still recognizes touches in the (currently blank)
// button area even while hidden. The touch that reveals the strip is
// consumed by the reveal itself, not treated as an actual button press
// (the user couldn't see what they were pressing yet). It auto-hides again
// after kButtonStripHideMs of inactivity, but only while the menu is closed
// -- you need the buttons visible to navigate it.
namespace {
bool buttonStripVisible = true;
absolute_time_t buttonStripHideDeadline;
constexpr std::uint32_t kButtonStripHideMs = 5000;

// Last commanded state of each status LED, so re-showing the strip (which
// redraws the whole cached bitmap, including their baked-in "off" look)
// can immediately reapply whichever state was actually current.
bool usbLedOn = false;
bool pilLedOn = false;
}  // namespace

void showButtonStrip() {
    if (buttonStripVisible) return;

    // Draw first, while the active window is still the full panel width
    // (that's what it was left at while hidden) -- the RA8875 clips
    // drawing to the active window, so drawing the strip at its screen
    // position *after* narrowing the window back down would get clipped
    // away since that position is outside the narrower window.
    display->drawBitmap565(static_cast<std::int16_t>(buttonStripScreenX0), 0,
                           buttonStripWidth, buttonStripHeight,
                           buttonStripPixels.data());
    hp82163::setStatusLed(display, hp82163::StatusLed::Usb, usbLedOn);
    hp82163::setStatusLed(display, hp82163::StatusLed::Pil, pilLedOn);

    // Now narrow the active window/text area back and let Screen reflow
    // into it.
    display->setActiveWindow(0, 0, SCREEN_MAX_X - buttonStripWidth - 1, SCREEN_MAX_Y - 1);
    screen->setTextWidth(SCREEN_MAX_X - buttonStripWidth);

    buttonStripVisible = true;
}

void hideButtonStrip() {
    if (!buttonStripVisible) return;

    // Widen the active window to the full panel, then let Screen reflow
    // into it -- full()'s own hardware clear wipes the button pixels, and
    // the wider layout naturally uses that space for text afterward.
    display->setActiveWindow(0, 0, SCREEN_MAX_X - 1, SCREEN_MAX_Y - 1);
    screen->setTextWidth(SCREEN_MAX_X);

    buttonStripVisible = false;
}

// ── Top-left-corner info box ────────────────────────────────────────────────
//
// Touching the top-left corner of the screen shows a summary of the current
// config. Reuses Screen's own suspend()/resume() -- same mechanism the menu
// already relies on -- so incoming HP-41 stream bytes still update the text
// buffer while the box is up, and resume() catches the display back up the
// moment it's dismissed, instead of losing anything.
namespace {
bool infoBoxVisible = false;
absolute_time_t infoBoxHideDeadline;
constexpr std::uint32_t kInfoBoxShowMs = 5000;
constexpr std::uint16_t kInfoCornerSize = 120;  // touch hit-zone: top-left NxN
}  // namespace

bool isInfoCornerTouch(std::uint16_t x, std::uint16_t y) {
    return x < kInfoCornerSize && y < kInfoCornerSize;
}

// Named color if it matches one of the ColorPicker menu's presets,
// otherwise falls back to the raw hex value.
const char* colorName(std::uint16_t c) {
    switch (c) {
        case 0xFFFF: return "White";
        case 0xFFE0: return "Yellow";
        case 0x07E0: return "Green";
        case 0x07FF: return "Cyan";
        case 0xF800: return "Red";
        default:     return nullptr;
    }
}

void showInfoBox() {
    if (infoBoxVisible) return;
    screen->suspend();

    constexpr int boxW = 400, boxH = 220;
    constexpr int boxX = (SCREEN_MAX_X - boxW) / 2;
    constexpr int boxY = (SCREEN_MAX_Y - boxH) / 2;
    hp82163::MenuFrame::draw(display, boxX, boxY, boxW, boxH);

    display->txtColor(0xFFFF, 0x0000);
    char buf[64];

    display->txtSize(1);
    std::snprintf(buf, sizeof(buf), "HIPI-Board %s", HIPI_VERSION);
    display->txtSetCursor(boxX + 20, boxY + 16);
    display->txtWrite(buf);

    display->txtSize(0);
    int y = boxY + 56;
    constexpr int lineStep = 24;

    if( hpilDevices > 0 ) {
        std::snprintf(buf, sizeof(buf), "Devices: %d", hpilDevices);
    } else {
        std::snprintf(buf, sizeof(buf), "Devices: (not configured)");
    }
    display->txtSetCursor(boxX + 20, y); display->txtWrite(buf); y += lineStep;

    std::snprintf(buf, sizeof(buf), "File: %s",
                  config.filename().empty() ? "(none)" : config.filename().c_str());
    display->txtSetCursor(boxX + 20, y); display->txtWrite(buf); y += lineStep;

    std::snprintf(buf, sizeof(buf), "Trace: %s",
                  config.debug() ? "Extended" : (config.trace() ? "On" : "Off"));
    display->txtSetCursor(boxX + 20, y); display->txtWrite(buf); y += lineStep;

    std::snprintf(buf, sizeof(buf), "Font size: %u", config.fontSize());
    display->txtSetCursor(boxX + 20, y); display->txtWrite(buf); y += lineStep;

    if (config.columns() == 0) std::snprintf(buf, sizeof(buf), "Columns: Auto");
    else                       std::snprintf(buf, sizeof(buf), "Columns: %u", config.columns());
    display->txtSetCursor(boxX + 20, y); display->txtWrite(buf); y += lineStep;

    const char* cname = colorName(config.textColor());
    if (cname) std::snprintf(buf, sizeof(buf), "Text color: %s", cname);
    else       std::snprintf(buf, sizeof(buf), "Text color: 0x%04X", config.textColor());
    display->txtSetCursor(boxX + 20, y); display->txtWrite(buf); y += lineStep;

    infoBoxVisible = true;
    infoBoxHideDeadline = make_timeout_time_ms(kInfoBoxShowMs);
}

void hideInfoBox() {
    if (!infoBoxVisible) return;
    screen->resume();  // catches up on anything the HP-41 stream sent meanwhile
    infoBoxVisible = false;
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

    // Vänta tills USB-CDC är ansluten (max 3 sekunder)
    while (!time_reached(timeout)) {
        tud_task();  // driver USB-stacken (host requests, enumeration, IN/OUT)
        if (tud_mounted()) {
            usb_connected = true;
            //ledPower.on();
            break;
        }
        sleep_ms(10);   // lite snällare än tight_loop_contents
    }

    // Wait for a terminal to actually open the CDC port too (max 2 seconds --
    // previously unbounded, so boot would hang forever with no USB present).
    absolute_time_t cdcTimeout = make_timeout_time_ms(2000);
    while (!tud_cdc_n_connected(0) && !time_reached(cdcTimeout)) {
        tud_task();
        sleep_ms(10);
    }

    ledA.off();

    // PIL status LED is left for you to drive from wherever "HP-IL is
    // active/connected" actually means in your setup, e.g.:
    //   pilLedOn = true;
    //   hp82163::setStatusLed(display, hp82163::StatusLed::Pil, pilLedOn);
    // (update pilLedOn too, not just the LED itself -- showButtonStrip()
    // reapplies it from that variable after being hidden and shown again)

    // First here we know if the debug-port is open or not (usb_connected)
    LOGF("HIPI Board v0.1\r\n");
    LOGF("======================");
    LOGF("\r\n * Init display ...");
    LOGF("\r\n * Init SD-card ... ");
    if (FR_OK != fr) {
        LOGF(" PANIC: f_mount error: %s (%d)\r\n", FRESULT_str(fr), fr);
    } else {
        LOGF("\r\n\t* SD-card mounted!");
    }

    tud_task();

    // Show buttons ...
    LOGF("\r\n * Draw buttons ... ");
    if (!drawBmpRightAligned(display, "buttons.bmp", SCREEN_MAX_X, 0,
                             &buttonStripPixels, &buttonStripWidth,
                             &buttonStripHeight, &buttonStripScreenX0))
        LOGF("\r\n ### Failed to draw buttons ... ");
    buttonStripHideDeadline = make_timeout_time_ms(kButtonStripHideMs);

    LOGF("\r\n * Draw text ... ");
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
        switch( (config.trace() ? 0b10 : 0b00) | (config.debug() ? 0b01 : 0b00) ) {
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

    usbLedOn = usb_connected;
    hp82163::setStatusLed(display, hp82163::StatusLed::Usb, usbLedOn);

    tud_cdc_n_write_flush(0);
    tud_task();

    dialog = new hp82163::UiDialog(display, *screen);
    dialog->setColorChangedCallback([](std::uint16_t c) { config.setTextColor(c); });
    dialog->setTraceChangedCallback([](bool t, bool d) { config.setTraceMode(t, d); });
    dialog->setFontSizeChangedCallback([](std::uint8_t s) { config.setFontSize(s); });
    dialog->setBrightnessChangedCallback([](std::uint8_t b) { config.setBrightness(b); });
    dialog->setColumnsChangedCallback([](std::uint8_t c) { config.setColumns(c); });

    // Init touch sensor ...
    LOGF("\r\n * Init touch sensor ...");
    touchInit();
    LOGF("\r\n\t* GSL1680 Boot up completed!\r\n");
    tud_task();

    // Init HPIL scanner ...
    LOGF("\r\n * Init HPIL interface ...");

    // Init HPIL scanner ...
    HpIlLoop hpil(IN_M_PIN, IN_P_PIN, OUT_M_PIN, OUT_P_PIN);

    // Setup all devices in the HPIL loop (display, drive, LEDs, PILBox)
    hipi_init();

    LOGF("\r\n\t* HP-IL initialized");
    // Done! Start the HPIL monoitoring ...
    LOGF("\r\n-----------------------------");
    LOGF("\r\nUp and running ...\r\n");

    bool bRunning = true;

    while (!time_reached(infoTimeout)) {
        tud_task();
        sleep_ms(10);
    }

    screen->clear();
    screen->setTextSize(config.fontSize());
    if (config.columns() != 0) screen->setColumns(config.columns());
    //screen->refreshCursor();
    //screen->clear();

    while (bRunning) {
        tud_task();  // TinyUSB background task
        hipi_loop(hpil);
        // Snabb reaktion vid NYTT tryck (interrupt-flaggan satts av IRQ_PIN)
        if (g_dataReadyFlag.exchange(false, std::memory_order_relaxed)) {
            if (!touchActive) {
                std::uint16_t tx, ty;
                if (touch_get_point(tx, ty)) {
                    touchActive = true;

                    if (infoBoxVisible) {
                        // Any touch while it's up dismisses it early --
                        // consumed here, not treated as a button-strip
                        // wake/press or another corner-tap.
                        hideInfoBox();
                    } else if (isInfoCornerTouch(tx, ty)) {
                        showInfoBox();
                    } else {
                    // Any touch, anywhere, keeps the strip up a while longer
                    // (or wakes it up if it was hidden). hitTestButton()
                    // works on fixed coordinates, so it still recognizes a
                    // touch in the button area even while nothing is drawn
                    // there -- that's what lets a "blind" tap wake it too.
                    const bool wasHidden = !buttonStripVisible;
                    if (wasHidden) showButtonStrip();
                    buttonStripHideDeadline = make_timeout_time_ms(kButtonStripHideMs);

                    hp82163::Button b = hp82163::hitTestButton(tx, ty);
                    // The touch that just woke the strip up is consumed by
                    // the reveal itself -- don't also act on it as a press,
                    // since the user couldn't see what they were touching.
                    if (!wasHidden && b != hp82163::Button::None) {
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
                        // The redraws above went through RA8875::drawBitmap565(),
                        // which switches to graphics mode (gfxMode()) and blindly
                        // zeros MWCR0 -- the same register that holds the cursor-
                        // visible bit. Screen never sees this happen (these calls
                        // bypass it entirely), so the blinking cursor would
                        // otherwise vanish on every single button touch.
                        screen->refreshCursor();
                        dialog->handleButton(b);
                    }
                    }
                }
            }
            // Om touchActive redan ar true: ignorera — fingret ligger kvar,
            // vantar bara pa att lyftas innan nasta tryck raknas.
        }

        // Auto-hide the info box after kInfoBoxShowMs, same idea as the
        // button strip's own countdown below.
        if (infoBoxVisible && time_reached(infoBoxHideDeadline)) {
            hideInfoBox();
        }

        // Auto-hide the strip after kButtonStripHideMs of inactivity -- but
        // never while the menu is open; you need the buttons visible to
        // navigate it.
        if (buttonStripVisible && !dialog->isOpen() &&
            time_reached(buttonStripHideDeadline)) {
            hideButtonStrip();
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
                    screen->refreshCursor();  // same MWCR0-clobber fix as on press
                    pressedButton = hp82163::Button::None;
                }
            }
        }
        tight_loop_contents();
    }

    LOGF("Done!\r\n");

}



bool drawBmpRightAligned(hp82163::RA8875* display, const char* path,
                         std::uint16_t screen_width, std::uint16_t y0,
                         std::vector<std::uint16_t>* outPixels,
                         std::uint16_t* outWidth,
                         std::uint16_t* outHeight,
                         std::uint16_t* outScreenX0) {
    std::uint16_t width = 0, height = 0;
    if (!hp82163::peekBmpDimensions(path, width, height)) {
        LOGF("\r\n\t * Could not read BMP dimensions for <%s>!", path);
        return false;
    }
    const std::uint16_t x0 = static_cast<std::uint16_t>(screen_width - width);  // höger kant
    if (outScreenX0) *outScreenX0 = x0;
    return hp82163::drawBmpAt(display, path, static_cast<std::int16_t>(x0),
                              static_cast<std::int16_t>(y0),
                              outPixels, outWidth, outHeight);
}
