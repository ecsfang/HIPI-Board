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

#include "hpil_pio.hpp"

#include "PicoSpiTransport.hpp"
#include "RA8875.hpp"
#include "Screen.hpp"
#ifdef TEST_DISPLAY
#include "display_test.hpp"
#endif

#include "hpil_debug_simple.pio.h"  // Enkel version

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
//#include "hardware/regs/io_bank0.h"   // för GPIO-register-adresser

#include <cstdio>

#define PICO_DEFAULT_LED

constexpr uint LED_PIN = 22;

#ifdef PICO_DEFAULT_LED
    // Pico 2 W — använd CYW43-HAL:en
//    #include "pico/cyw43_arch.h"
//    #define BLINK_LED_TYPE_WIFI
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

inline void runLoopbackTest() {
    printf("\n=== HP-IL PIO Loopback Test ===\n");
    printf("Wiring: GP14→GP12, GP15→GP13\n\n");

    // Initiera HP-IL med dina pin-numbers
    HpIlLoop hpil(/*minus_in=*/13, /*plus_in=*/12,
                  /*minus_out=*/15, /*plus_out=*/14);

    printf("1. Initialized\n");
    printf("   RX SM: %u, TX SM: %u\n", hpil.sm_rx(), hpil.sm_tx());

    // Hjälpfunktion för att testa
    auto test_roundtrip = [&](uint32_t value, const char* label) -> bool {
        printf("\n2. Test '%s': sending 0x%08X\n", label, value);
        
        // Skicka
        if (!hpil.try_write(value)) {
            printf("   FAIL: TX FIFO full\n");
            return false;
        }
        printf("   TX: wrote 0x%08X to FIFO\n", value);
        
        // Vänta lite
        sleep_ms(50);
        
        // Läs
        uint32_t rx = 0;
        if (!hpil.try_read(rx)) {
            printf("   FAIL: RX FIFO empty (no loopback)\n");
            return false;
        }
        
        printf("   RX: got 0x%08X\n", rx);
        
        bool match = (rx == value);
        printf("   %s: %s\n",
               label,
               match ? "PASS ✓" : "FAIL ✗ (data mismatch)");
        return match;
    };

    // Test 1: enkelt värde
    bool ok1 = test_roundtrip(0x12345678, "Simple value");

    // Test 2: alla ettor
    bool ok2 = test_roundtrip(0xFFFFFFFF, "All ones");

    // Test 3: alla nollor
    bool ok3 = test_roundtrip(0x00000000, "All zeros");

    // Test 4: växlande mönster
    bool ok4 = test_roundtrip(0xAAAAAAAA, "Alternating");
    bool ok5 = test_roundtrip(0x55555555, "Anti-alternating");

    // Test 5: Kontinuerlig data (för logikanalysator)
    printf("\n3. Stress test: sending 16 frames...\n");
    int success = 0;
    for (int i = 0; i < 16; i++) {
        uint32_t value = 0x10000000 | (i << 16) | i;
        hpil.try_write(value);
        sleep_ms(20);
        
        uint32_t rx;
        if (hpil.try_read(rx)) {
            if (rx == value) {
                printf("   Frame %2d: 0x%08X ↔ 0x%08X ✓\n", i, value, rx);
                success++;
            } else {
                printf("   Frame %2d: TX=0x%08X RX=0x%08X ✗\n", i, value, rx);
            }
        } else {
            printf("   Frame %2d: TX=0x%08X (no RX) ✗\n", i, value);
        }
    }
    printf("   Success: %d/16\n", success);

    printf("\n=== Summary ===\n");
    printf("Test 1 (simple):   %s\n", ok1 ? "PASS" : "FAIL");
    printf("Test 2 (ones):     %s\n", ok2 ? "PASS" : "FAIL");
    printf("Test 3 (zeros):    %s\n", ok3 ? "PASS" : "FAIL");
    printf("Test 4 (alt):      %s\n", ok4 && ok5 ? "PASS" : "FAIL");
    printf("Stress test:       %d/16 passed\n", success);

    if (!ok1 && !ok2 && !ok3 && !ok4) {
        printf("\n*** No RX data at all ***\n");
        printf("Likely causes:\n");
        printf("  - Wiring: GP14→GP12, GP15→GP13\n");
        printf("  - TX PIO doesn't run: check pio_sm_get_pc(pio0, sm_tx)\n");
        printf("  - RX PIO doesn't run: check pio_sm_get_pc(pio0, sm_rx)\n");
        printf("  - Pin config wrong: check pio_gpio_init order\n");
    } else if (!ok1) {
        printf("\n*** Some RX but mismatches ***\n");
        printf("Likely causes:\n");
        printf("  - Bit-order mismatch (shift direction?)\n");
        printf("  - Timing: PIO is too slow/fast\n");
        printf("  - Pull-ups: maybe GP12/13 need pull-downs\n");
    } else {
        printf("\n*** All PASS — HP-IL PIO works! ***\n");
    }
}


static bool usb_connected = false;

int main() {
    stdio_init_all();
    sleep_ms(2000);
#define OUT_PIN 15
#if 0
    printf("Blinking LED on GP22\n");
    
    // Initiera GP22 som output
    gpio_init(22);
    gpio_set_dir(22, GPIO_OUT);
    
    // Blink forever
    while (true) {
        gpio_put(22, 1);   // LED ON
        sleep_ms(500);
        gpio_put(22, 0);   // LED OFF
        sleep_ms(500);
    }
#else
    printf("PIO test on GP2 (SPI SCK)\n");
    printf("Connect LED+resistor from GP2 to GND\n");
    printf("Expected: LED blinks\n\n");
    
    // Använd GP2 (SPI SCK) — lätt att testa
    HpIlDebug1Hz debug(22);
    
    printf("TX SM: %u\n", debug.sm());
    
    // Skriv ut PC varje sekund
    for (int i = 0; i < 10; i++) {
        printf("PC t+%d: %d\n", i, pio_sm_get_pc(pio0, debug.sm()));
        sleep_ms(1000);
    }
    printf("GP14 function: %d (should be 6 = PIO)\n", gpio_get_function(22));

    printf("\nDone. LED should have blinked.\n");
    
    while (true) {
        tight_loop_contents();
    }
#endif
#if 0
    HpIlDebug1Hz debug(/*out_pin=*/15);
    
    printf("SM: %u, PC: %d\n", debug.sm(), 
           pio_sm_get_pc(pio0, debug.sm()));
    
    for (int i = 0; i < 10; i++) {
        sleep_ms(1000);
        printf("PC t+%d: %d\n", i, 
               pio_sm_get_pc(pio0, debug.sm()));
    }
    
    while (true) {
        tight_loop_contents();
    }
#endif
//#ifdef BLINK_LED_TYPE_WIFI
//    if (cyw43_arch_init() != 0) {
//        // Init misslyckades — blinka med en utprintad felkod istället
//        while (true) {
//            printf("CYW43 init failed\n");
//            sleep_ms(1000);
//        }
//    }
//#else
//    gpio_set_dir(LED_PIN, GPIO_OUT);
//#endif
//
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

#ifdef TEST_DISPLAY
    display.begin();
    display.set8Bpp();
    display.set2LayerConfig();
    
    // Kör diagnostiska tester
    hp82163::runDisplayTests(display);
#else
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

#endif

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

    // Init HPIL scanner ...
    if( usb_connected ) {
        printf("\n * Init HPIL interface ...");
    }

//    HpIlLoop hpil(/*minus_in=*/13, /*plus_in=*/12,
  //                /*minus_out=*/15, /*plus_out=*/14);

    if( usb_connected ) {
        printf(" HP-IL initialized");
    }
    // Done! Start the HPIL monoitoring ...
    if( usb_connected ) {
        printf("\n-----------------------------");
        printf("\nUp and running ...\n");
        printf("Run HPIL tests ...\n");
    }

    runLoopbackTest();

    while(1) {}


    //   hipi_tests();

    if( usb_connected ) {
        printf("Done!\n");
    }

    while (true) {
        led_task(to_ms_since_boot(get_absolute_time()));
        tight_loop_contents();
    }
}
