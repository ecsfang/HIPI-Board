#include "leds.h"


void led_on(int n) {
    gpio_put(n, 1);
}
void led_off(int n) {
    gpio_put(n, 0);
}

void blink_led(int led, int t, int n) {
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

// Physical order: index 0 = leftmost … 4 = rightmost
constexpr uint8_t LED_PINS[5] = {
    LED_PIN_1, LED_PIN_2, LED_PIN_3, LED_PIN_4, LED_PIN_5
};

static inline uint32_t ms_now()              { return to_ms_since_boot(get_absolute_time()); }
static inline void     ms_sleep(uint32_t ms) { sleep_ms(ms); }

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

PicoPwm::PicoPwm(uint8_t pin) {
    this->pin = pin;
    this->slice_num = pwm_gpio_to_slice_num(this->pin);
    this->channel = pwm_gpio_to_channel(this->pin);

    gpio_set_function(this->pin, GPIO_FUNC_PWM);
}

PicoPwm::~PicoPwm() { this->stop(); }

void PicoPwm::setFrequency(uint32_t freq) {
    uint32_t source_hz = clock_get_hz(clk_sys);
    uint32_t div16_top = 16 * source_hz / freq;
    uint32_t top = 1;
    while (1) {
        // Try a few small prime factors to get close to the desired frequency.
        if (div16_top >= 16 * 5 && div16_top % 5 == 0 && top * 5 <= TOP_MAX) {
            div16_top /= 5;
            top *= 5;
        } else if (div16_top >= 16 * 3 && div16_top % 3 == 0 && top * 3 <= TOP_MAX) {
            div16_top /= 3;
            top *= 3;
        } else if (div16_top >= 16 * 2 && top * 2 <= TOP_MAX) {
            div16_top /= 2;
            top *= 2;
        } else {
            break;
        }
    }
    if (div16_top < 16) {
        printf("Freq too low!\n!");
    } else if (div16_top >= 256 * 16) {
        printf("Freq too high!\n!");
    }

    this->div = div16_top;
    this->top = top - 1;

    pwm_set_clkdiv_int_frac(this->slice_num, this->div / 16, this->div & 0xF);
    pwm_set_wrap(this->slice_num, this->top);

    this->setDuty(this->duty);
}

void PicoPwm::setDuty(uint32_t duty) {
    uint32_t cc = duty * (this->top + 1) / 65535;
    pwm_set_chan_level(this->slice_num, this->channel, cc);
    pwm_set_enabled(this->slice_num, true);
    this->duty = duty;
}

void PicoPwm::setDutyPercentage(uint8_t percentage) {
    uint32_t duty = (1 << 16) * (percentage / 100.0);
    this->setDuty(duty);
}

void PicoPwm::setInverted(bool inverted_a, bool inverted_b) { pwm_set_output_polarity(this->slice_num, inverted_a, inverted_b); }

void PicoPwm::stop() { pwm_set_enabled(this->slice_num, false); }

uint8_t PicoPwm::getPin() { return this->pin; }
uint8_t PicoPwm::getSlice() { return this->slice_num; }
uint8_t PicoPwm::getChannel() { return this->channel; }


void pwm_test(int pin)
{
    PicoPwm pwm0 = PicoPwm(pin);

    pwm0.setFrequency(25e3);  // set 25khz frequency

    int p;

    for(int n=0; n<10; n++) {
        for(p = 0; p<=100; p++) {
            pwm0.setDutyPercentage(p);  // 30% (0%-100%)
            ms_sleep(10);
            // pwm0.setDuty(256); 
        }
        for(p = 0; p<=100; p++) {
            pwm0.setDutyPercentage(100-p);  // 30% (0%-100%)
            ms_sleep(10);
            // pwm0.setDuty(256); 
        }
    }
}