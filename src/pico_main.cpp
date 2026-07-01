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

#include "usb_serial.h"


extern void init_spi(void);

#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "pico/time.h"


#include "tusb.h" 
//#include "pico/stdio_usb.h"

#include <cstdio>

//#define PICO_DEFAULT_LED

//constexpr uint LED_PIN = 22;

extern bool SDOK;
extern void sd_dir();

#define stdio_usb_connected() tud_cdc_n_connected(0)


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

void led_on(int n) {
    gpio_put(n, 1);
}
void led_off(int n) {
    gpio_put(n, 0);
}

void blink_led(int led, int t=250, int n=1) {
    for( int i=0; i<n; ++i) {
        // LED PÅ
        led_on(led);
        sleep_ms(t);

        // LED AV
        led_off(led);
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

void led_task(int led, uint32_t now_ms)
{
    if (now_ms < next_time)
        return;

    led_state = !led_state;

    if (led_state)
        led_on(led);
    else
        led_off(led);

    next_time = now_ms + pattern[state] + (rand() % 40);

    state++;
    if (state >= sizeof(pattern)/sizeof(pattern[0]))
        state = 0;
}


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

/*
 * an improved format is using full 8-bit bytes:
 * 001c ccb0 high byte: higher 4 bits (3 control bits plus data bit 7)
 * 1bbb bbbb low byte: lo
 * When a frame has to be transmited to the serial link, the driver will check if the high byte (control bits and
 * data bit 7) is the same than the previously transmited frame. If yes, it is not transmited again and only the
 * low byte is sent.
 * Conversely, if the receiver gets only the low byte, it will use the last received high byte to rebuild the full
 * frame.
 */

uint16_t PIL_rx_lo;                 // PILBox lo byte previously received   
uint16_t PIL_rx_hi;                 // PILBox hi byte previously received
uint16_t PIL_tx_lo;                 // PILBox lo byte previously sent   
uint16_t PIL_tx_hi;                 // PILBox hi byte previously sent
uint16_t PIL_rx_frame;              // PILBox frame just received
uint16_t PIL_rx_prevframe;          // PILBox previous frame received
uint16_t PIL_tx_prevframe;          // PILBox previous frame sent

IL_CMD_t send2PC(IL_CMD_t cmd)
{
    unsigned char bHi, bLo;
    char bIn[2];
    static unsigned char prevHi = 0;
    // ccc bbbb bbbb
    bHi = ((cmd>>6) & 0x1E) | 0b00100000;            // b1 = 001c ccb0
    bLo = ((cmd & 0x7F))    | 0b10000000;            // b2 = 1bbb bbbb

    if (!tud_cdc_n_connected(1))
        return cmd;

    if( bHi == prevHi ) {
        tud_cdc_n_write(1, &bLo, 1);
        tud_cdc_n_write_flush(1);
    } else {
        tud_cdc_n_write(1, &bHi, 1);
        tud_cdc_n_write(1, &bLo, 1);
        tud_cdc_n_write_flush(1);
    }

    int n = cdc1_read_timeout(&bIn[0], 2, 500);
    switch( n ) {
    case 2:
        cdc0_printf("B1: %02X B2: %02X\r\n", bIn[0], bIn[1]);
        break;
    case 1:
        cdc0_printf("B1: %02X ", bIn[0]);
        n = cdc1_read_timeout(&bIn[0], 2, 500);
        cdc0_printf("n = %d B2: %02X\r\n", n, bIn[0]);
        break;
    default:
        cdc0_printf("n = %d !!!\r\n", n);

    }

   
    if (!tud_cdc_n_connected(1))
    {
        // no valid serial link, loopback mode 
        frame = loopbackFrame;
        loopbackFrame = 0xFFFF;             // to indicate no new frame is available
        return frame;                       // and get out
    }
    else if (tud_cdc_n_available(1) == 0)
    {
        // no bytes available
        return 0xFFFF;                      // return no data and get out
    }
    else
    {
        // we get here when:
        // - there is a valid serial link
        // - and there is data available in the serial buffer
        // if a frame arrives we must check for a PILBox command first
        pil_recv = tud_cdc_n_read_char(1);

        // PILBox emulation received a byte from the PILBox designated serial port
        // pil_recv contains the returned byte
        if ((pil_recv & 0xE0) == 0x20)
        {
            // this is the higher byte of a transfer
            PIL_tx_hi = pil_recv;       // save until the lower byte arrives
            return 0xFFFF;              // and return with no data
        }
        if ((pil_recv & 0x80) == 0x80)
        {
            // this is the lower byte of an 8-bit transfer
            PILmode8 = true;                    // set the correct mode to 8 bits
            PIL_rx_lo = pil_recv;               

            // this completes the 2-byte transfer, complete the frame
            PIL_rx_frame = (pil_recv & 0x7F) | ((PIL_tx_hi & 0x1E) << 6);
        }
         if ((pil_recv & 0xC0) == 0x40)
        {
            // this is the lower byte of a 7-bit transfer
            PILmode8 = false;            // set the correct mode
            PIL_rx_lo = pil_recv;       
              
            // this completes the 2-byte transfer, complete the frame
            PIL_rx_frame = (pil_recv & 0x3F) | ((PIL_tx_hi & 0x1F) << 6);
        }

        // The frame is now received, first process the PILBox commands
        // send to our scope for debugging
        // PILBox_scope(PIL_rx_frame, PIL_tx_hi, pil_recv, false);

        switch (PIL_rx_frame)
        {
            case TDIS:                          // TDI: Translator DIsabled
                PILBox_mode = TDIS;             // set mode to disabled
                                                // frame is not forwarded to the HP-IL emulation
                tud_cdc_n_write_char(1, pil_recv);       // return command for confirmation
                tud_cdc_n_write_flush(1);
                // return 0xFFFF;                  // and return with no data
                break;
            case CON:                           // CON: Controller ON
                PILBox_mode = CON;              // set mode to controller ON
                                                // default on the HP41
                                                // frame is not forwarded to the HP-IL emulation
                tud_cdc_n_write_char(1, pil_recv);       // return command for confirmation
                tud_cdc_n_write_flush(1);
                return 0xFFFF;                  // and return with no data
                break;
            case COFF:                          // Controller OFF
                PILBox_mode = COFF;             // set mode to controller OFF
                                                // the PILBox is now a device
                                                // not used on the HP41
                                                // frame is not forwarded to the HP-IL emulation
                tud_cdc_n_write_char(1, pil_recv);       // return command for confirmation
                tud_cdc_n_write_flush(1);
                return 0xFFFF;               // and return with no data
                break;
            case COFI:                          // Controller OFf with IDY 
                PILBox_mode = COFI;             // set mode to COFI
                                                // device with sending IDY frame
                                                // frame is not forwarded to the HP-IL emulation
                tud_cdc_n_write_char(1, pil_recv);       // return command for confirmation
                tud_cdc_n_write_flush(1);
                return 0xFFFF;                  // and return with no data
                break;
            // default:

                // all other frames are sent on to the HP-IL loop
                // only need to check for a CMD frame
                // should do a check if the mode is TDIS, any traffic should be ignored
                // if (PIL_rx_frame == m_wLastFrame)
                // {
                    // this is a previous CMD frame, this is returned as an RFC frame
                //     PIL_rx_frame = RFC;
                // }
                // else if (PIL_rx_frame == RFC)
                // {
                //     PIL_rx_frame = m_wLastFrame;
                // }
        }
        // if we get here the frame is complete
        return PIL_rx_frame;
    }
}
    return cmd;
}

hp82163::Screen *screen;


int main() {
    //stdio_init_all();
    // Replace stdio_init_all() with:
    usb_init();
    sleep_ms(500);

    alienBegin();
    alienStartup(2000);

    absolute_time_t timeout = make_timeout_time_ms(1000);

    // Vänta tills USB-CDC är ansluten (max 3 sekunder)
    while (!time_reached(timeout)) {
        if (stdio_usb_connected()) {
            usb_connected = true;
            break;
        }

        blink_led(LED_PIN_1, 250);
        tight_loop_contents();
    }

//    pwm_test(LED_PIN_5);


    // SPI0: SCK=GP2, MOSI=GP3, MISO=GP0 (matches share.py)
    gpio_set_function(2, GPIO_FUNC_SPI);   // SCK
    gpio_set_function(3, GPIO_FUNC_SPI);   // MOSI
    gpio_set_function(0, GPIO_FUNC_SPI);   // MISO

//    breathing_led::init(LED_PIN_3); 

    // CS=GP1 and RST=GP4 are configured by PicoSpiTransport's constructor.
    if( usb_connected ) {
        cdc0_printf("HIPI Board v0.1\n");
        cdc0_printf("======================");
        cdc0_printf("\n * Init display ...");
    }

    // Interface 1 — new second port
    const char* msg = "Hello from CDC1 (/dev/ttyACM1)\n";
    cdc1_write(msg, strlen(msg));

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
#ifdef DISP_TEST
    for (int r=0; r<32; r++) {
        const char *line = "This is row #";
        for (const char* p = line; *p; ++p) screen->pr_char(*p);
        if( r >= 10 )
            screen->pr_char((r/10)+'0');
        screen->pr_char((r%10)+'0');
        screen->pr_char('\r');
        screen->pr_char('\n');
        ms_sleep(500);
    }
    for(int i=0; i<5;i++) {
        screen->pr_char(27);
        screen->pr_char('A');
        ms_sleep(500);
    }
    for(int i=0; i<5;i++) {
        screen->pr_char(27);
        screen->pr_char('C');
        ms_sleep(500);
    }
    for(int i=0; i<5;i++) {
        screen->pr_char(27);
        screen->pr_char('B');
        ms_sleep(500);
    }
    for(int i=0; i<5;i++) {
        screen->pr_char(27);
        screen->pr_char('D');
        ms_sleep(500);
    }
    screen->pr_char(27);
    screen->pr_char('S');
    ms_sleep(500);
    screen->pr_char(27);
    screen->pr_char('T');
    ms_sleep(500);
    screen->pr_char(27);
    screen->pr_char('H');
    ms_sleep(500);
    for(int i=0; i<10;i++) {
        screen->pr_char(27);
        screen->pr_char('B');
        ms_sleep(500);
    }
    screen->pr_char(27);
    screen->pr_char('J');
    ms_sleep(500);
    screen->pr_char(27);
    screen->pr_char('%');
    screen->pr_char(15);
    screen->pr_char(15);
    screen->pr_char(64);
    screen->pr_char(64);
    screen->pr_char(64);
    ms_sleep(500);
#endif

#endif

    // Init SD-card ...
    if( usb_connected ) {
        cdc0_printf("\n * Init SD-card ... ");
    }
    init_spi();
    FATFS fs;
    FRESULT fr = f_mount(&fs, "", 1);
    if (FR_OK != fr) {
        cdc0_printf(" PANIC: f_mount error: %s (%d)\n", FRESULT_str(fr), fr);
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
        cdc0_printf("\n * Init touch sensor ...");
        touchTest();
    }

    // Init HPIL scanner ...
    if( usb_connected ) {
        cdc0_printf("\n * Init HPIL interface ...");
    }

    HpIlLoop hpil(IN_M_PIN, IN_P_PIN, OUT_M_PIN, OUT_P_PIN);

    hipi_init();

    if( usb_connected ) {
        cdc0_printf(" HP-IL initialized");
    }
    // Done! Start the HPIL monoitoring ...
    if( usb_connected ) {
        cdc0_printf("\n-----------------------------");
        cdc0_printf("\nUp and running ...\n");
    }

    while (true) {
        usb_task();
        int cmd = hipi_loop(hpil);
        cmd = send2PC(cmd);
//        breathing_led::update(cmd);
        tight_loop_contents();
    }

    if( usb_connected ) {
        cdc0_printf("Done!\n");
    }

}
