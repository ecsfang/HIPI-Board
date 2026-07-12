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
//#include "buttons.h"
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

#include <cstdio>

//#define PICO_DEFAULT_LED

//constexpr uint LED_PIN = 22;

extern bool SDOK;
extern void sd_dir();

bool drawBmpRightAligned(hp82163::RA8875& display, const char* path,
                         std::uint16_t screen_width, std::uint16_t y0);
#ifdef UI_DIALOG
namespace {
bool touchActive = false;
absolute_time_t lastReleasePoll = get_absolute_time();
}  // namespace
#endif

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

    sleep_ms(2000);

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
    }
    tud_task();

    // Interface 1 — new second port
    //const char* msg = "Hello from CDC1 (/dev/ttyACM1)\r\n";
    //cdc1_write(msg, strlen(msg));


    for(int i=0; i<10; i++) {
        tud_task();
        sleep_ms(20);
    }

    hp82163::PicoSpiTransport transport(spi0,
                                        /*baudrate=*/6'000'000,
                                        /*cs_gpio=*/1,
                                        /*rst_gpio=*/4);

    hp82163::RA8875 display(transport, /*width=*/800, /*height=*/480);

#ifdef UI_DIALOG
    hp82163::UiDialog dialog(display, *screen);
#endif

    display.begin();

    display.set8Bpp();

    // Show buttons ...
    cdc0_printf("\r\n * Draw buttons ... ");
    if (!drawBmpRightAligned(display, "buttons.bmp", 800, 0))
        cdc0_printf("\r\n ### Failed to draw buttons ... ");

    cdc0_printf("\r\n * Draw text ... ");
    display.setActiveWindow(0, 0, 679, 479);
    screen = new hp82163::Screen(display, FONT_COLOR, TEXT_SIZE, BRIGHTNESS, 680 );

    const char* lines[] = {
        "HELLO, WORLD!",
        "HP82163 EMULATOR",
        "PICO 2 + RA8875",
        "PICO 2 + RA8875",
        "PICO 2 + RA8875",
    };
    for (const char* line : lines) {
        for (const char* p = line; *p; ++p) screen->pr_char(*p);
        screen->pr_char('\r');
        screen->pr_char('\n');
    }    

    tud_cdc_n_write_flush(0);
    tud_task();

    // Init touch sensor ...
    if( usb_connected ) {
        cdc0_printf("\r\n * Init touch sensor ...");
        touchTest();
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
#ifdef UI_DIALOG
        // Snabb reaktion vid NYTT tryck (interrupt-flaggan satts av IRQ_PIN)
        if (g_dataReadyFlag.exchange(false, std::memory_order_relaxed)) {
            if (!touchActive) {
                std::uint16_t tx, ty;
                if (touch_get_point(tx, ty)) {
                    touchActive = true;
                    hp82163::Button b = hp82163::hitTestButton(tx, ty);
                    if (b != hp82163::Button::None) dialog.handleButton(b);
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
            if (!touch_is_down()) touchActive = false;
       }
#endif
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
                         std::uint16_t screen_width, std::uint16_t y0) {
    FIL file;

    cdc0_printf("\r\n\t * Open file <%s> ... ", path);

    FRESULT fr =f_open(&file, path, FA_READ);
    if (fr != FR_OK) {
        cdc0_printf(" PANIC: f_mount error: %s (%d)\r\n", FRESULT_str(fr), fr);
        return false;
    }
    cdc0_printf("\r\n\t * Read file ... ");

    std::uint8_t header[54];
    UINT br = 0;
    if (f_read(&file, header, 54, &br) != FR_OK || br != 54 ||
        header[0] != 'B' || header[1] != 'M') {
        cdc0_printf("\r\n\t * No BMP file!");
        f_close(&file);
        return false;
    }

    auto rd32 = [&](int off) -> std::int32_t {
        return static_cast<std::int32_t>(
            header[off] | (header[off+1] << 8) |
            (header[off+2] << 16) | (header[off+3] << 24));
    };
    auto rd16 = [&](int off) -> std::int16_t {
        return static_cast<std::int16_t>(header[off] | (header[off+1] << 8));
    };

    const std::uint32_t dataOffset  = static_cast<std::uint32_t>(rd32(10));
    const std::int32_t  width       = rd32(18);
    const std::int32_t  heightRaw   = rd32(22);
    const std::int16_t  bpp         = rd16(28);
    const std::int32_t  compression = rd32(30);

    cdc0_printf("\r\n\t * Width: %d height: %d", width, heightRaw);
    cdc0_printf("\r\n\t * bpp: %d", bpp);

    // Vi stöder bara det png_to_bmp24.py genererar: okomprimerad 24-bit.
    if (bpp != 24 || compression != 0 || width <= 0) {
        cdc0_printf("\r\n\t * Wrong format!");
        f_close(&file);
        return false;
    }

    const bool topDown = heightRaw < 0;
    const std::uint32_t height  = static_cast<std::uint32_t>(topDown ? -heightRaw : heightRaw);
    const std::uint32_t rowSize = ((static_cast<std::uint32_t>(width) * 3 + 3) / 4) * 4;  // 4-byte padding

    const std::uint16_t x0 = static_cast<std::uint16_t>(screen_width - width);  // höger kant

    std::vector<std::uint8_t>  rawRow(rowSize);
    std::vector<std::uint8_t> rowBuf(static_cast<std::size_t>(width));

    for (std::uint32_t r = 0; r < height; ++r) {
        // BMP lagras normalt underifrån och upp om height är positiv.
        const std::uint32_t fileRow = topDown ? r : (height - 1 - r);

        f_lseek(&file, dataOffset + static_cast<FSIZE_t>(fileRow) * rowSize);
        if (f_read(&file, rawRow.data(), rowSize, &br) != FR_OK || br != rowSize) {
            cdc0_printf("\r\n\t * Error ... ?");
            f_close(&file);
            return false;
        }

        for (std::int32_t col = 0; col < width; ++col) {
            const std::uint8_t b = rawRow[static_cast<std::size_t>(col) * 3 + 0];
            const std::uint8_t g = rawRow[static_cast<std::size_t>(col) * 3 + 1];
            const std::uint8_t rr = rawRow[static_cast<std::size_t>(col) * 3 + 2];
            // RA8875 8bpp-format: RRRGGGBB
            rowBuf[static_cast<std::size_t>(col)] =
            static_cast<std::uint8_t>((rr & 0xE0) | ((g & 0xE0) >> 3) | (b >> 6));
            //rowBuf[static_cast<std::size_t>(col)] = static_cast<std::uint16_t>(
            //    ((rr & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
        }

        display.drawBitmap332(static_cast<std::int16_t>(x0),
                              static_cast<std::int16_t>(y0 + r),
                              static_cast<std::uint16_t>(width), 1,
                              rowBuf.data());
        //display.drawBitmap565(static_cast<std::int16_t>(x0),
        //                      static_cast<std::int16_t>(y0 + r),
        //                      static_cast<std::uint16_t>(width), 1,
        //                      rowBuf.data());
    }

    cdc0_printf("\r\n\t * Done!");
    f_close(&file);
    return true;
}
