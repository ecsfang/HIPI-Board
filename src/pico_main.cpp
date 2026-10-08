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

#define HIPI_VERSION_TEXT "3.2beta"

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
#include "screendump.h"
#include "bootscreen.h"
#include "boot_service.h"
#include "backlight.h"
#include "clock.h"
#include "loopmap.h"
#include "chartable.h"
#include "hpil_diag.h"
#include "pilbox.h"      // the PILBox row on the start-up screen
#include "analyzer.h"
#include "display_mirror.h"   // the display mirrored to a PC (tools/hipiview)
#include "config.hpp"
#include "drive.h"
extern std::vector<CDevice*> devices;   // hipi.cpp -- the HP-IL devices, loop order

#include <cstdio>

hipi::Config config;

// Run the loop as long as true ...
bool bRunning = true;

extern "C" bool tud_vendor_control_xfer_cb(uint8_t /*rhport*/,
                                            uint8_t stage,
                                            tusb_control_request_t const* request) {
    if (stage != CONTROL_STAGE_SETUP) return true;
    if (request->bmRequestType == 0x40 && request->bRequest == 0x01) {
        reset_usb_boot(0, 0);
    }
    return true;
}


// ─── Compatibility shim ───────────────────────────────────────────────────────
#include "pico/stdlib.h"

namespace {
// FONT_COLOR/TEXT_SIZE/BRIGHTNESS used to be hardcoded here; the defaults
// now live in Config.hpp and are overridden by CONFIG.TXT on the SD card
// once one exists.
constexpr const char* HIPI_VERSION = HIPI_VERSION_TEXT;  // shown on splash screen
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
#if defined(DISPLAY_7INCH) && HIPI_MIRROR_TRANSPORT
    // Everything sent to the display also goes to the display mirror port
    // while a PC viewer has it open (display_mirror.h)
    static hipi::MirrorTransport mirror(*transport);
    display = new hipi::DisplayDriver(mirror, SCREEN_MAX_X, SCREEN_MAX_Y);
#else
    display = new hipi::DisplayDriver(*transport, SCREEN_MAX_X, SCREEN_MAX_Y);
#endif

    display->begin();
    hipi::displayMirror_init(display);
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
        pilbox_setEarlyEnabled(config.isDeviceEnabled("PILBOX"));
        bExtTrace = config.extTrace();
    }
    return fr;
}

#ifdef DISPLAY_7INCH
// Start-up screen rows: SD card and settings (right after mounting)
static void bootRowsSdAndSettings(FRESULT fr) {
    char buf[40];
    if (fr != FR_OK) {
        hipi::bootscreen_row("SD card", hipi::BootState::Fail, "not found");
        hipi::bootscreen_row("Settings", hipi::BootState::Info, "defaults");
        return;
    }
    // Card size in tenths of a GB (integer maths -- no float printf needed)
    const std::uint64_t bytes = static_cast<std::uint64_t>(fs.n_fatent - 2) * fs.csize * 512u;
    const unsigned tenths = static_cast<unsigned>(bytes / 100000000ull);
    const char* type = fs.fs_type == FS_EXFAT ? "exFAT" : fs.fs_type == FS_FAT32 ? "FAT32"
                     : fs.fs_type == FS_FAT16 ? "FAT16" : "FAT12";
    std::snprintf(buf, sizeof(buf), "%u.%u GB %s", tenths / 10, tenths % 10, type);
    hipi::bootscreen_row("SD card", hipi::BootState::Ok, buf);
    FILINFO fi;
    const bool haveConfig = f_stat("CONFIG.TXT", &fi) == FR_OK;
    hipi::bootscreen_row("Settings", haveConfig ? hipi::BootState::Ok : hipi::BootState::Info,
                         haveConfig ? "CONFIG.TXT" : "defaults");
}

// The PILBox row: "waiting (TDIS)" until the PC's program has done its
// handshake, then "connected (COFF)" etc.; "off" if switched off
static const char* pilboxModeName(IL_CMD_t m) {
    switch (m) {
        case COFF: return "COFF";
        case COFI: return "COFI";
        case CON:  return "CON";
        default:   return "TDIS";
    }
}
static std::string pilboxStatus() {
    if (pilbox == nullptr || !pilbox->enabled()) return "disabled";
    if (!pilbox->isConnected()) return "waiting (TDIS)";
    return std::string("connected (") + pilboxModeName(pilbox->mode()) + ")";
}
static hipi::BootState pilboxState() {
    return (pilbox != nullptr && pilbox->enabled() && pilbox->isConnected()) ? hipi::BootState::Ok
                                                                             : hipi::BootState::Off;
}

// Rows after the devices exist: Tape view, each drive's cassette, trace,
// and the DEVICES card
static void bootRowsDevices() {
    const bool tape = hipi::plotterview_isAvailable(hipi::DisplayOutput::Tape);
    hipi::bootscreen_row("Bitmaps", tape ? hipi::BootState::Ok : hipi::BootState::Fail,
                         tape ? "images loaded" : "images missing");
    for (CDevice* dev : devices) {
        if (dev->type() != DRIVE) continue;
        const std::string& f = static_cast<CDrive*>(dev)->mediaFile();
        hipi::bootscreen_row(dev->name(), f.empty() ? hipi::BootState::Off : hipi::BootState::Ok,
                             f.empty() ? "No media" : f.c_str());
    }
    hipi::bootscreen_row("Trace", config.trace() ? hipi::BootState::Info : hipi::BootState::Off,
                         config.extTrace() ? "extended" : (config.trace() ? "on" : "off"));
    hipi::bootscreen_devices(devices);
}
#endif

int main() {
    board_init(); 
    tusb_init();
    // Stay invisible to the PC until HIPI is ready (tud_connect() just
    // before the start-up screen's wait loop): a PC program such as pyILPER
    // starts its PILBox handshake the moment the port shows up, and gives
    // up after a short timeout. Logging meanwhile goes to the boot-log
    // buffer (usb_serial.h) and is sent once a terminal connects.
    tud_disconnect();

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
#ifdef DISPLAY_7INCH
    // Start-up screen (bootscreen.h): its STATUS rows are filled in below
    // as start-up gets to each step
    hipi::bootscreen_begin(display, HIPI_VERSION);
    bootRowsSdAndSettings(fr);
#else
    hipi::showSplashScreen(display, HIPI_VERSION, 2000);
#endif

    // (USB is connected late, see tud_connect() before the wait loop)

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
        hipi_bootService();
        sleep_ms(20);
    }

    screen->pr_char(27);
    screen->pr_char('<'); // Cursor off
#ifndef DISPLAY_7INCH
    // 5": the classic text summary (the 7" panel has the start-up screen)
    screen->pr_str("# HIPI - HP-IL Pico Interface #");

    // (USB connects at the end of start-up -- its state is on the status LED)
    {
        char buf[64];
        for (CDevice* dev : devices) {       // each cassette drive's file
            if (dev->type() != DRIVE) continue;
            const std::string& f = static_cast<CDrive*>(dev)->mediaFile();
            snprintf(buf, sizeof(buf), " * %s: %.32s", dev->name(), f.empty() ? "No media" : f.c_str());
            screen->pr_str(buf);
        }
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
#endif

    // 10s, not 5 -- gives enough time to actually read the device
    // self-check list (see hipi_test()) that ends up on screen just
    // before this; a touch breaks out of the wait early (see below).
    absolute_time_t infoTimeout = make_timeout_time_ms(10000);

    tud_cdc_n_write_flush(0);
    tud_task();

    dialog = new hipi::UiDialog(display, *screen);
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

#ifdef DISPLAY_7INCH
    hipi::bootscreen_row("HP-IL", hipi::BootState::Ok, "loop ready");
#if HIPI_DEV_SCREENDUMPS
    // Development aid (see screendump.h): the start-up screen halfway
    // through its STATUS rows, for the user manual
    {
        std::string msg;
        const bool ok = hipi::screendump_save(display, msg, /*withOverlays=*/true);
        LOGF("\r\n * DEV screendump (start-up screen): %s%s", ok ? "saved " : "FAILED -- ", msg.c_str());
    }
#endif
#endif

    // Setup all devices in the HPIL loop (display, drive, LEDs, PILBox)
    hipi_init();

    // Wire up the plotter's live-draw callbacks now that display/screen/
    // plotter all exist (plotter is set inside hipi_init() above).
    hipi::plotterview_init(display, screen, plotter);
    hipi::plotterview_setPlotFit(config.plotFit());   // Display -> Plot view (saved)
    hipi::plotterview_setPaperBlack(config.plotPaperBlack());  // Plotter -> Paper (saved)
    hipi::analyzer_init(display);            // HP-IL analyzer (Display -> Analyzer)
    hipi::loopmap_init(display);             // HP-IL loop map (Display -> Loop map)
    hipi::chartable_init(display);           // System -> Character table
    hipi::hpil_diag_init(display);           // HP-IL signals view (scope, Display -> HP-IL signals)
    hipi::hpil_diag_setBitCells(static_cast<hipi::BitCells>(config.signalCells()));   // saved
    hipi::displayMirror_setEnabled(config.displayMirror());   // Settings -> Screen -> Mirror to PC (saved)
    hipi::clock_init(display);               // DS3231 clock, clock screen saver
    if (CDrive* d = hipi::plotterview_drive())               // cassette in the Tape view
        hipi::plotterview_setTapeFile(d->mediaFile());
    // Project escape sequences from the HP-41 (see
    // Screen::setExtCommandCallback()) -- only recorded here, carried out
    // from the main loop by boardui_poll():
    //   "ESC # D" / "ESC # M"  screen dump without / with the menus and
    //                          other overlays
    //   "ESC # V <addr>"       show the view of the HIPI device at HP-IL
    //                          address <addr> (one byte, 1..30)
    screen->setExtCommandCallback([](std::uint8_t c, std::uint8_t arg) {
        if (c == 'D') hipi::screendump_request(false);
        else if (c == 'M') hipi::screendump_request(true);
        else if (c == 'V') hipi::plotterview_requestViewByAddress(arg);
    }, "V");

#ifdef DISPLAY_7INCH
    bootRowsDevices();
#endif
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

    // Now HIPI is ready: show up on USB. HP-IL (and with it PILBox) runs
    // right away -- the PC's first PILBox command is answered at once.
    tud_connect();
    {
        const absolute_time_t usbTimeout = make_timeout_time_ms(1500);
        while (!time_reached(usbTimeout)) {
            hipi_bootService();
            hipi_loop(hpil);
            if (tud_mounted()) { usb_connected = true; break; }
        }
    }
#ifdef DISPLAY_7INCH
    hipi::bootscreen_row("USB", usb_connected ? hipi::BootState::Ok : hipi::BootState::Off,
                         usb_connected ? "connected" : "not connected");
    // PILBox: the PC's program usually connects during the countdown, so
    // this row is kept up to date in the wait loop below
    std::string pilboxShown = pilboxStatus();
    const int pilboxRow = hipi::bootscreen_row("PILBox", pilboxState(), pilboxShown.c_str());
#endif

#ifdef DISPLAY_7INCH
    // The start-up screen stays for kBootScreenMs (a touch skips it), with
    // a countdown bar
    constexpr std::uint32_t kBootScreenMs = 20000;
    absolute_time_t bootWaitStart = get_absolute_time();
    infoTimeout = make_timeout_time_ms(kBootScreenMs);
#if HIPI_DEV_SCREENDUMPS
    bool devReadyDumped = false;
#endif
#endif
    // HP-IL (and with it PILBox, for the PC) runs already while the
    // start-up screen is shown -- the loop must not be dead for that time
    absolute_time_t nextTouchCheck = nil_time;
    while (!time_reached(infoTimeout)) {
        hipi_bootService();
        usb_serial_flush_boot_log();
        const bool hadFrame = hipi_loop(hpil);
#ifdef DISPLAY_7INCH
        {
            const std::string now = pilboxStatus();
            if (now != pilboxShown) {
                pilboxShown = now;
                hipi::bootscreen_updateRow(pilboxRow, pilboxState(), now.c_str());
            }
        }
        const float bootFraction = static_cast<float>(absolute_time_diff_us(bootWaitStart, get_absolute_time()))
                                   / (kBootScreenMs * 1000.0f);
        hipi::bootscreen_progress(bootFraction);
#if HIPI_DEV_SCREENDUMPS
        // Development aid (see screendump.h): the finished start-up screen
        // with the countdown bar halfway, for the user manual. Saving takes
        // a few seconds -- the countdown then continues from halfway.
        if (!devReadyDumped && bootFraction >= 0.5f) {
            devReadyDumped = true;
            std::string msg;
            const bool ok = hipi::screendump_save(display, msg, /*withOverlays=*/true);
            LOGF("\r\n * DEV screendump (start-up screen, ready): %s%s", ok ? "saved " : "FAILED -- ", msg.c_str());
            bootWaitStart = from_us_since_boot(to_us_since_boot(get_absolute_time()) - kBootScreenMs * 500ull);
            infoTimeout = make_timeout_time_ms(kBootScreenMs / 2);
        }
#endif
#endif
        // A raw touch check (not the full touch_poll()/tap-callback
        // flow) -- lets someone skip the wait early without also
        // processing this same touch as a real tap, which would show
        // the button strip right as boot finishes.
        // (Checked every 10 ms: an I2C read, too slow for every frame)
        if (time_reached(nextTouchCheck)) {
            nextTouchCheck = make_timeout_time_ms(10);
            if (touch_is_down()) break;
        }
        if (!hadFrame) sleep_us(200);
    }

#ifdef DISPLAY_7INCH
    hipi::bootscreen_end();
#endif
    screen->clear();
    screen->setTextSize(config.fontSize());
    if (config.columns() != 0) screen->setColumns(config.columns());

    hipi::backlight_init(screen);   // the screen saver timer starts now (backlight.h)
    hipi_bootServiceDone();   // the main loop handles USB and PILBox from here
    while (bRunning) {
        tud_task();                     // TinyUSB background task
        usb_serial_flush_boot_log();    // flush buffered boot messages once a terminal connects
        hipi_loop(hpil);                // Check IL for any instruction to send through the loop!
        touch_poll();                   // debounced tap/release detection (touch.h)
        hipi::boardui_poll();        // auto-hide timers + status LED poll
        hipi::plotterview_poll();    // view-switch splash auto-dismiss timer
        hipi::backlight_poll();      // screen saver (dim / clock) after a while without use
        hipi::displayMirror_poll();  // the display mirrored to a PC viewer (display_mirror.h)
        hipi::clock_poll();          // clock screen saver; "time" command on the USB console
#if HIPI_ANALYZER
        hipi::analyzer_poll();       // HP-IL analyzer: analyse captured frames, redraw
        hipi::loopmap_poll();        // HP-IL loop map: learn from the traffic, redraw
        hipi::hpil_diag_poll();      // HP-IL signals view: scope captures while shown
#endif

        tight_loop_contents();
    }

    LOGF("Done!\r\n");

}
