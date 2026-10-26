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
CLedDriver ledAct   (LED_PINS[3]);   // LED 4 (D8): HP-IL activity or general purpose
CLedDriver ledB     (LED_PINS[4]);   // LED 5 (D7): general purpose, or the NeoPixels' data line

CLedDriver* leds[] = { &ledPower, &ledStatus, &ledError, &ledAct, &ledB };
CLedParser  parser(leds, 5);

// ─── HP-IL activity LED (see leds.h) ──────────────────────────────────────
namespace {
int hpilLedMode_ = kHpilLedMessages;
bool hpilLedsHeld_ = false;
std::uint64_t hpilLastUs_ = 0;
constexpr std::uint32_t kHpilLitMs = 30;    // lit this long after a frame
}  // namespace

void hpil_led_activity(bool idle) {
    if (hpilLedMode_ == kHpilLedOff || hpilLedsHeld_) return;
    if (idle && hpilLedMode_ == kHpilLedMessages) return;      // (IDY: the loop only idling)
    // At most one (re)start every 10 ms: a frame costs only a time read
    const std::uint64_t now = time_us_64();
    if (now - hpilLastUs_ < 10000) return;
    hpilLastUs_ = now;
    leds[HPIL_ACT_LED]->blink(kHpilLitMs, 1, 1);   // indicate a frame
}

void hpil_leds_setMode(int mode) {
    if (mode < kHpilLedOff || mode > kHpilLedMessages) mode = kHpilLedMessages;
    if (mode == hpilLedMode_) return;
    const bool wasOn = hpilLedMode_ != kHpilLedOff;
    hpilLedMode_ = mode;
    // Off: LED 4 given back dark (it may be in the middle of a flash)
    if (wasOn && mode == kHpilLedOff && !hpilLedsHeld_) leds[HPIL_ACT_LED]->off();
}

int hpil_leds_mode() { return hpilLedMode_; }

// ─── The plotter's ERROR light on LED 3 (see leds.h) ──────────────────────
namespace {
bool plotLedOn_ = true;
int plotLedShown_ = -1;                    // as last written: 0 / 1, -1 = not yet (write it)
}  // namespace

void plotter_led_setOn(bool on) {
    if (on == plotLedOn_) return;
    plotLedOn_ = on;
    // Off: given back dark; on: written at the next poll
    if (!on && plotLedShown_ == 1 && !hpilLedsHeld_) leds[PLOT_ERR_LED]->off();
    plotLedShown_ = -1;
}

bool plotter_led_on() { return plotLedOn_; }

void plotter_led_poll(bool error) {
    if (!plotLedOn_) return;
    if (hpilLedsHeld_) { plotLedShown_ = -1; return; }    // (written again once given back)
    const int want = error ? 1 : 0;
    if (want == plotLedShown_) return;
    plotLedShown_ = want;
    if (error) leds[PLOT_ERR_LED]->on();
    else leds[PLOT_ERR_LED]->off();
}

void hpil_leds_hold(bool held) { hpilLedsHeld_ = held; }

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

    ledAct.fadeOn(1000);              // starts from current brightness (0) → 100%

    while (!ledAct.isIdle())
        tight_loop_contents();

    // ledAct is now steady at 100%


    // ── Example 5: Fade to a level, then slow pulse ───────────────────────────

    ledB.setBrightness(60);
    ledB.fadeTo(60, 500);           // fade from 0 → 60% over 500 ms

    while (!ledB.isIdle())
        tight_loop_contents();

    ledB.blink(50, 950);            // brief flash once per second at 60%


    // ── Example 6: Chained sequence on ledAct ────────────────────────────────
    //
    //  FIX: use ledAct (currently steady on) — not ledStatus which is already
    //  in use. ledAct is IDLE at 100% so fadeOn goes 100→100 (instant hold),
    //  so we explicitly fade off first to make the sequence visible.
    //
    //  Sequence: fade out → blink 4 times → fade back in

    enum class Seq { FADE_OUT, BLINK, FADE_IN, DONE } seq = Seq::FADE_OUT;

    ledAct.fadeOff(600);              // start: fade from 100% → 0

    while (seq != Seq::DONE) {
        if (ledAct.isIdle()) {
            switch (seq) {
                case Seq::FADE_OUT:
                    ledAct.blink(150, 150, 4);
                    seq = Seq::BLINK;
                    break;
                case Seq::BLINK:
                    ledAct.fadeOn(800);
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

    // ledAct is back on at 100%, sequence complete


    // ── Main loop ─────────────────────────────────────────────────────────────
    //
    //  The hardware timer handles all LED updates automatically — nothing
    //  LED-related is needed here.
 }