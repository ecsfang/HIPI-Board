#include "leds.h"
#include "usb_serial.h"


void led_on(int n) {
    gpio_put(n, 1);
}
void led_off(int n) {
    gpio_put(n, 0);
}

void blink_led(int led, int t, int n) {
    for( int i=0; i<n; ++i) {
        // LED ON
        led_on(led);
        sleep_ms(t);

        // LED OFF
        led_off(led);
        sleep_ms(t);
    }
}

static const uint16_t pattern[] = {
    60,  80,   // blink
    60, 300,   // blink
    120, 70,   // blink
    40,  900,  // long pause
    50,  50,   // double blink
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
uint8_t LED_PINS[5] = {
    LED_PIN_1, LED_PIN_2, LED_PIN_3, LED_PIN_4, LED_PIN_5
};


#ifndef CLED_NO_PWM

PicoPwm::PicoPwm(uint8_t pin) {
    this->pin      = pin;
    this->slice_num = pwm_gpio_to_slice_num(pin);
    this->channel   = pwm_gpio_to_channel(pin);

    gpio_set_function(pin, GPIO_FUNC_PWM);

    this->top = TOP_MAX;                          // 65534
    pwm_set_wrap(this->slice_num, this->top);     // tell hardware
}

PicoPwm::~PicoPwm() { this->stop(); }

void PicoPwm::setFrequency(uint32_t freq) {
    uint32_t source_hz = clock_get_hz(clk_sys);
    uint32_t div16_top = 16 * source_hz / freq;
    uint32_t top = 1;
    while (1) {
        // Try a few small prime factors to get close to the desired frequency.
        if (div16_top >= 16 * 5 && div16_top % 5 == 0 && top * 5 <= static_cast<uint32_t>(TOP_MAX)) {
            div16_top /= 5;
            top *= 5;
        } else if (div16_top >= 16 * 3 && div16_top % 3 == 0 && top * 3 <= static_cast<uint32_t>(TOP_MAX)) {
            div16_top /= 3;
            top *= 3;
        } else if (div16_top >= 16 * 2 && top * 2 <= static_cast<uint32_t>(TOP_MAX)) {
            div16_top /= 2;
            top *= 2;
        } else {
            break;
        }
    }
    if (div16_top < 16) {
        LOGF("Freq too low!\n!");
    } else if (div16_top >= 256 * 16) {
        LOGF("Freq too high!\n!");
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
    if (percentage >= 100) {
        setDuty(65535);     // full on, no overflow risk
        return;
    }
    uint32_t duty = 65535u * percentage / 100;
    setDuty(duty);
}

void PicoPwm::setInverted(bool inverted_a, bool inverted_b) { pwm_set_output_polarity(this->slice_num, inverted_a, inverted_b); }

void PicoPwm::stop() { pwm_set_enabled(this->slice_num, false); }

uint8_t PicoPwm::getPin() { return this->pin; }
uint8_t PicoPwm::getSlice() { return this->slice_num; }
uint8_t PicoPwm::getChannel() { return this->channel; }

#endif//CLED_NO_PWM

// ─── Create all five LEDs ─────────────────────────────────────────────────────
//
//  Declare them globally (or as static locals in main) so they live for the
//  entire program. The shared timer starts automatically with the first
//  instance and runs until the last one is destroyed.
//
//  GPIO assignments — adjust to your wiring:

CLedDriver ledPower (LED_PINS[0]);   // Power indicator
CLedDriver ledStatus(LED_PINS[1]);   // Status / activity
CLedDriver ledError (LED_PINS[2]);   // Error / alert
CLedDriver ledA     (LED_PINS[3]);   // General purpose A
CLedDriver ledB     (LED_PINS[4]);   // General purpose B

CLedDriver* leds[] = { &ledPower, &ledStatus, &ledError, &ledA, &ledB };
CLedParser  parser(leds, 5);

// ─────────────────────────────────────────────────────────────────────────────

void ledTest() {

    // ── Example 1: On / off / brightness ─────────────────────────────────────

    ledPower.setBrightness(40);     // set level before turning on
    ledPower.on();                  // FIX: don't call off() — led stays on at 40%


    // ── Example 2: Blink exactly N times, then steady on ─────────────────────

    ledStatus.blink(100, 150, 3);   // 3 flashes

    while (!ledStatus.isIdle())     // wait for all 3 to complete
        tight_loop_contents();

    ledStatus.on();                 // steady on afterwards


    // ── Example 3: Infinite blink ("waiting" indicator) ──────────────────────

    ledError.setBrightness(100);
    ledError.blink(200, 800);       // count defaults to -1 = infinite

    // To stop it later:  ledError.off();


    // ── Example 4: Fade in on boot ───────────────────────────────────────────

    ledA.fadeOn(1000);              // starts from current brightness (0) → 100%

    while (!ledA.isIdle())
        tight_loop_contents();

    // ledA is now steady at 100%


    // ── Example 5: Fade to a level, then slow pulse ───────────────────────────

    ledB.setBrightness(60);
    ledB.fadeTo(60, 500);           // fade from 0 → 60% over 500 ms

    while (!ledB.isIdle())
        tight_loop_contents();

    ledB.blink(50, 950);            // brief flash once per second at 60%


    // ── Example 6: Chained sequence on ledA ──────────────────────────────────
    //
    //  FIX: use ledA (currently steady on) — not ledStatus which is already
    //  in use. ledA is IDLE at 100% so fadeOn goes 100→100 (instant hold),
    //  so we explicitly fade off first to make the sequence visible.
    //
    //  Sequence: fade out → blink 4 times → fade back in

    enum class Seq { FADE_OUT, BLINK, FADE_IN, DONE } seq = Seq::FADE_OUT;

    ledA.fadeOff(600);              // start: fade from 100% → 0

    while (seq != Seq::DONE) {
        if (ledA.isIdle()) {
            switch (seq) {
                case Seq::FADE_OUT:
                    ledA.blink(150, 150, 4);
                    seq = Seq::BLINK;
                    break;
                case Seq::BLINK:
                    ledA.fadeOn(800);
                    seq = Seq::FADE_IN;
                    break;
                case Seq::FADE_IN:
                    seq = Seq::DONE;
                    break;
                default: break;
            }
        }
        tight_loop_contents();
    }

    // ledA is back on at 100%, sequence complete


    // ── Main loop ─────────────────────────────────────────────────────────────
    //
    //  The hardware timer handles all LED updates automatically — nothing
    //  LED-related is needed here.
 }