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
#ifdef TEST_DISPLAY
#include "display_test.hpp"
#endif

#include "hpil_debug_simple.pio.h"  // Enkel version

// SD-card support
#include "ff.h"
#include "f_util.h"
#include "hw_config.h"
#include "hpil.h"

extern void init_spi(void);

#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "hardware/pwm.h"
#include "pico/time.h"
//#include "hardware/regs/io_bank0.h"   // för GPIO-register-adresser

#include <cstdio>

//#define PICO_DEFAULT_LED

//constexpr uint LED_PIN = 22;

extern bool SDOK;
extern void sd_dir();

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
static inline uint32_t ms_now()              { return to_ms_since_boot(get_absolute_time()); }
static inline void     ms_sleep(uint32_t ms) { sleep_ms(ms); }

// ─── Pin definitions (edit here) ─────────────────────────────────────────────
constexpr uint LED_PIN_1 = 4;
constexpr uint LED_PIN_2 = 5;
constexpr uint LED_PIN_3 = 6;
constexpr uint LED_PIN_4 = 22;
constexpr uint LED_PIN_5 = 21;

// Physical order: index 0 = leftmost … 4 = rightmost
constexpr uint8_t LED_PINS[5] = {
    LED_PIN_1, LED_PIN_2, LED_PIN_3, LED_PIN_4, LED_PIN_5
};

// ─── Tiny xorshift32 PRNG (no seed needed – deterministic but visually random)
static uint32_t xr32_state = 0xDEADBEEFUL;

static uint32_t xr32_next() {
    xr32_state ^= xr32_state << 13;
    xr32_state ^= xr32_state >> 17;
    xr32_state ^= xr32_state << 5;
    return xr32_state;
}

// Returns a pseudo-random value in [lo, hi] (inclusive).
static uint32_t xr32_range(uint32_t lo, uint32_t hi) {
    return lo + (xr32_next() % (hi - lo + 1));
}

// ─── Helper: configure all LED pins as digital outputs ───────────────────────
/**
 * alienBegin()
 *
 * Initialises every LED pin as a digital output and turns all LEDs off.
 * Call this once from setup() / main() before alienStartup().
 */
void alienBegin() {
    for (uint8_t i = 0; i < 5; ++i) {
        gpio_init(LED_PINS[i]);
        gpio_set_dir(LED_PINS[i], GPIO_OUT);
        gpio_put(LED_PINS[i], 0);
    }
}

// ─── Helper: turn all LEDs off ────────────────────────────────────────────────
/**
 * allOff()
 *
 * Drives all five LED pins LOW.
 */
void allOff() {
    for (uint8_t i = 0; i < 5; ++i) {
        gpio_put(LED_PINS[i], 0);
    }
}

// ─── Helper: apply a 5-bit bitmask to the LEDs ───────────────────────────────
/**
 * setPattern(uint8_t bits)
 *
 * Sets each LED according to the corresponding bit in `bits` (bits 0–4).
 * Bit 0 → LED_PIN_1 (leftmost), bit 4 → LED_PIN_5 (rightmost).
 *
 * @param bits  5-bit pattern, range 0–31.
 */
void setPattern(uint8_t bits) {
    for (uint8_t i = 0; i < 5; ++i) {
        bool on = (bits >> i) & 0x01;
        gpio_put(LED_PINS[i], on ? 1 : 0);
    }
}

// ─── Main boot sequence ───────────────────────────────────────────────────────
/**
 * alienStartup(uint32_t durationMs)
 *
 * Runs a five-phase "alien intelligence" LED boot animation that lasts
 * approximately `durationMs` milliseconds in total.
 *
 * Phase 1 – Wake-up pulse:
 *   The centre LED blinks three times, signalling system power-on.
 *
 * Phase 2 – Scanner sweep:
 *   A single lit LED bounces left↔right like a sensor calibrating itself.
 *
 * Phase 3 – Binary count:
 *   The LEDs count in binary from 1 to 31, simulating the CPU initialising.
 *
 * Phase 4 – Chaotic thinking:
 *   Random patterns (never 0, always ≥1 LED lit) flicker rapidly,
 *   representing active neural/computation bursts.
 *
 * Phase 5 – Login cascade:
 *   All LEDs illuminate together, then extinguish outward-to-inward in a
 *   smooth cascade — the system is ready.
 *
 * Time budget: each phase receives 1/5 of durationMs, checked every
 * iteration so the total stays within ±one iteration of durationMs.
 *
 * @param durationMs  Total desired duration in milliseconds (default 2000).
 */
void alienStartup(uint32_t durationMs) {
    const uint32_t start     = ms_now();
    const uint32_t phaseMs   = durationMs / 5;   // budget per phase
    uint32_t       phaseEnd  = start + phaseMs;

    // ── Phase 1: Centre wake-up pulse ────────────────────────────────────────
    // Bit pattern for centre LED only: bit 2 set → 0b00100 = 4
    for (int blink = 0; blink < 3 && ms_now() < phaseEnd; ++blink) {
        setPattern(0b00100);
        ms_sleep(80);
        allOff();
        ms_sleep(60);
    }

    // ── Phase 2: Scanner sweep left ↔ right ──────────────────────────────────
    phaseEnd = start + phaseMs * 2;
    {
        // Sweep positions: LED index 0..4, then 3..1 (bounce)
        const uint8_t sweep[] = {0, 1, 2, 3, 4, 3, 2, 1};
        const uint8_t sweepLen = sizeof(sweep);
        uint8_t idx = 0;

        while (ms_now() < phaseEnd) {
            setPattern(static_cast<uint8_t>(1 << sweep[idx]));
            ms_sleep(55);
            idx = (idx + 1) % sweepLen;
        }
        allOff();
    }

    // ── Phase 3: Binary count 1..31 ──────────────────────────────────────────
    phaseEnd = start + phaseMs * 3;
    {
        uint8_t val = 1;
        while (ms_now() < phaseEnd) {
            setPattern(val);
            ms_sleep(50);
            val = (val % 31) + 1;   // 1, 2, … 31, 1, 2, …
        }
        allOff();
    }

    // ── Phase 4: Chaotic blinking (never 0) ──────────────────────────────────
    phaseEnd = start + phaseMs * 4;
    {
        // Vary the delay for organic-looking bursts.
        while (ms_now() < phaseEnd) {
            uint8_t pattern = static_cast<uint8_t>(xr32_range(1, 31));
            setPattern(pattern);
            ms_sleep(static_cast<uint32_t>(xr32_range(20, 80)));
        }
    }

    // ── Phase 5: All on, then cascade off outward→inward ─────────────────────
    // Total time left from now until start + durationMs.
    phaseEnd = start + durationMs;
    {
        setPattern(0b11111);
        ms_sleep(180);

        // Cascade order: outermost pair first (0,4), then (1,3), then centre (2)
        const uint8_t cascade[][2] = {{0, 4}, {1, 3}, {2, 2}};
        const uint32_t stepMs = 120;

        uint8_t litBits = 0b11111;
        for (uint8_t step = 0; step < 3 && ms_now() < phaseEnd; ++step) {
            litBits &= ~(1 << cascade[step][0]);
            litBits &= ~(1 << cascade[step][1]);
            setPattern(litBits);
            ms_sleep(stepMs);
        }
        allOff();

        // Hold dark for the remainder of the phase budget.
        uint32_t now = ms_now();
        if (now < phaseEnd) {
            ms_sleep(phaseEnd - now);
        }
    }

    allOff();
}


namespace breathing_led {

static constexpr uint     PWM_WRAP     = 1000;
static constexpr uint     PWM_MAX      = PWM_WRAP;
static constexpr uint32_t BREATH_MS    = 1500;       // 1.5 s cykel — tydligare
static constexpr uint32_t HOLD_US      = 1'000'000;  // 1 s efter senaste 0x6C0

static uint16_t _breath_lut[256];

static uint     _slice_num;
static uint     _channel;
static uint32_t _x_last_time   = 0;
static uint32_t _breath_start  = 0;
static bool     _is_breathing  = false;

void init(uint gpio) {
    // Förberäkna smoothstep (256 entries, 0..PWM_MAX)
    for (int i = 0; i < 256; i++) {
        float t = (float)i / 255.0f;
        float s = t * t * (3.0f - 2.0f * t);
        _breath_lut[i] = (uint16_t)(s * PWM_MAX);
    }

    gpio_set_function(gpio, GPIO_FUNC_PWM);
    _slice_num = pwm_gpio_to_slice_num(gpio);
    _channel   = pwm_gpio_to_channel(gpio);

    // RP2350 default clk_sys = 150 MHz.
    // f_pwm = 150e6 / (clkdiv × (wrap+1))
    // Vi vill ha ~100 kHz PWM: 150e6 / 100e3 / 1001 ≈ 1.499
    pwm_set_wrap(_slice_num, PWM_WRAP);
    pwm_set_clkdiv(_slice_num, 1.5f);
    pwm_set_chan_level(_slice_num, _channel, 0);
    pwm_set_enabled(_slice_num, true);
}

static void set_brightness(uint16_t level) {
    pwm_set_chan_level(_slice_num, _channel, level);
}

static void update_breath(uint32_t now) {
    uint32_t phase_ms = (now - _breath_start) % BREATH_MS;
    int idx = (int)((uint32_t)phase_ms * 256u / BREATH_MS);
    uint16_t level = _breath_lut[idx];

    // DEBUG — skriv ut 5 ggr/sekund
    static uint32_t last_dbg = 0;
    if ((uint32_t)(now - last_dbg) > 200'000) {
        printf("    breath: phase=%u idx=%d level=%u\n",
               (unsigned)phase_ms, idx, level);
        last_dbg = now;
    }

    set_brightness(level);
}

void update(int value) {
    const uint32_t now = time_us_32();

    // DEBUG
    static int last_v = -1;
    if (value != last_v) {
        printf("v=0x%X now=%u _is_breathing=%d\n",
               value, (unsigned)now, (int)_is_breathing);
        last_v = value;
    }
    // /DEBUG

    if (value == 0x6C0) {
        _x_last_time = now;
        if (!_is_breathing) {
            _is_breathing = true;
            _breath_start = now;
            printf("  start breathing, _breath_start=%u\n", (unsigned)_breath_start);
        }
    }

    if (_is_breathing) {
        if ((uint32_t)(now - _x_last_time) >= HOLD_US) {
            _is_breathing = false;
            set_brightness(0);
        } else {
            update_breath(now);
        }
    }
}


} // namespace




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

hp82163::Screen *screen;

int main() {
    stdio_init_all();
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
#define LED_PIN LED_PIN_5
    breathing_led::init(LED_PIN);
    for (int i = 0; i < 10; i++) {
        pwm_set_chan_level(pwm_gpio_to_slice_num(LED_PIN),
                           pwm_gpio_to_channel(LED_PIN), 0);
        sleep_ms(500);
        pwm_set_chan_level(pwm_gpio_to_slice_num(LED_PIN),
                           pwm_gpio_to_channel(LED_PIN), 1000);
        sleep_ms(500);
    }

    // SPI0: SCK=GP2, MOSI=GP3, MISO=GP0 (matches share.py)
    gpio_set_function(2, GPIO_FUNC_SPI);   // SCK
    gpio_set_function(3, GPIO_FUNC_SPI);   // MOSI
    gpio_set_function(0, GPIO_FUNC_SPI);   // MISO

    breathing_led::init(LED_PIN_3); 

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
        printf("\n * Init SD-card ... ");
    }
    init_spi();
    FATFS fs;
    FRESULT fr = f_mount(&fs, "", 1);
    if (FR_OK != fr) {
        printf(" PANIC: f_mount error: %s (%d)\n", FRESULT_str(fr), fr);
        SDOK = false;
    } else {
        if( usb_connected ) {
            printf(" SD-card mounted!");
            sd_dir();
        }
        SDOK = true;
    }

    // Init touch sensor ...
    if( usb_connected ) {
        printf("\n * Init touch sensor ...");
        touchTest();
    }

    // Init HPIL scanner ...
    if( usb_connected ) {
        printf("\n * Init HPIL interface ...");
    }

    HpIlLoop hpil(IN_M_PIN, IN_P_PIN, OUT_M_PIN, OUT_P_PIN);

    hipi_init();

    if( usb_connected ) {
        printf(" HP-IL initialized");
    }
    // Done! Start the HPIL monoitoring ...
    if( usb_connected ) {
        printf("\n-----------------------------");
        printf("\nUp and running ...\n");
    }

    while (true) {
        int cmd = hipi_loop(hpil);
        breathing_led::update(cmd);
        tight_loop_contents();
    }

    if( usb_connected ) {
        printf("Done!\n");
    }

}
