#ifndef __LEDS_H__
#define __LEDS_H__

// PWM re-enabled: LED_PIN_4/5 moved off GPIO21/22, which aliased the exact
// same PWM slice+channel as LED_PIN_2 (GPIO5) and LED_PIN_3 (GPIO6) on the
// RP2040/RP2350 (slice = (gpio/2) % 8, channel = gpio % 2 -- GPIO n and
// GPIO n+16 always collide). GPIO26/27 land on their own free slice (5),
// so all five LEDs now have independent PWM channels.
//#define CLED_NO_PWM     // Due to HW conflict - now PWM right now!

#include <stdlib.h>
#include "pico/stdlib.h"
#include <stdio.h>
#include <pico/stdio.h>
#include <cstdint>
#include <string>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "pico/time.h"
#include "hardware/clocks.h"

#ifndef CLED_NO_PWM
#include "hardware/pwm.h"
#endif

#include <cstdio>

// ─── Pin definitions (edit here) ─────────────────────────────────────────────
#define LED_PIN_1 4
#define LED_PIN_2 5
#define LED_PIN_3 6
#define LED_PIN_4 21 //26
#define LED_PIN_5 22 //27

class PicoPwmBaseException : public std::exception {
   protected:
    std::string _msg;

   public:
    PicoPwmBaseException(std::string message = "PWM exception occurred") { _msg = message; };
    virtual const char* what() const throw() { return _msg.c_str(); }
};

class PicoPwmFreqTooLow : public PicoPwmBaseException {
   public:
    PicoPwmFreqTooLow(std::string message = "PWM Frequency too low.") : PicoPwmBaseException(message){};
};

class PicoPwmFreqTooHigh : public PicoPwmBaseException {
   public:
    PicoPwmFreqTooHigh(std::string message = "PWM Frequency too high.") : PicoPwmBaseException(message){};
};

class PicoPwm {
   protected:
    const int TOP_MAX = 65534;
    uint8_t pin = 0;
    uint8_t slice_num = 0;
    uint8_t channel = 0;
    uint32_t duty = 0;
    uint32_t div = 0;
    uint16_t top = 0;

   public:
    PicoPwm(uint8_t pin);
    ~PicoPwm();
    void setFrequency(uint32_t frequency);
    void setDuty(uint32_t duty);
    void setDutyPercentage(uint8_t percentage);
    void setInverted(bool inverted_a, bool inverted_b);

    uint8_t getPin();
    uint8_t getSlice();
    uint8_t getChannel();
    void stop();
};

extern void alienBegin(void);
extern void alienStartup(uint32_t durationMs);

namespace breathing_led {
extern void init(uint gpio);
extern void update(int value);
}

extern void pwm_test(int pin);

extern void led_on(int n);
extern void led_off(int n);
extern void blink_led(int led, int t=250, int n=1);
extern void led_task(int led, uint32_t now_ms);


// ─────────────────────────────────────────────────────────────────────────────
//  Define CLED_NO_PWM to use simple GPIO on/off instead of PWM.
//  Blink still works. Fade/setBrightness become instant on/off.
//  Remove the define (or comment it out) to restore full PWM support.
// ─────────────────────────────────────────────────────────────────────────────
class CLedDriver {
public:
    enum class Mode { IDLE, BLINKING, FADING };

    static constexpr uint8_t MAX_LEDS = 8;
    static constexpr uint8_t TICK_MS  = 10;

    // ── Construction / destruction ────────────────────────────────────────────

    explicit CLedDriver(uint gpio)
        : _gpio(gpio)
#ifndef CLED_NO_PWM
        , _pwm(gpio)
#endif
        , _brightness(100)
        , _currentPct(0.0f)
        , _mode(Mode::IDLE)
        , _blinkOnMs(500), _blinkOffMs(500)
        , _blinkTotal(-1), _blinkLeft(0)
        , _blinkPhaseMs(0), _blinkLit(false)
        , _fadeFrom(0.0f), _fadeTo(0.0f)
        , _fadeDurationMs(1), _fadeElapsedMs(0)
    {
#ifdef CLED_NO_PWM
        gpio_init(_gpio);
        gpio_set_dir(_gpio, GPIO_OUT);
        gpio_put(_gpio, 0);
#endif
        if (_count < MAX_LEDS)
            _instances[_count++] = this;

        if (_count == 1)
            add_repeating_timer_ms(-TICK_MS, _timerCB, nullptr, &_timer);
    }

    ~CLedDriver() {
        for (uint8_t i = 0; i < _count; i++) {
            if (_instances[i] == this) {
                _instances[i] = _instances[--_count];
                _instances[_count] = nullptr;
                break;
            }
        }
        if (_count == 0)
            cancel_repeating_timer(&_timer);
        off();
    }

    // ── Commands ──────────────────────────────────────────────────────────────

    void on() {
        _mode = Mode::IDLE;
        _apply((float)_brightness);
    }

    void off() {
        _mode = Mode::IDLE;
        _apply(0.0f);
    }

    // In CLED_NO_PWM mode: updates the stored level but has no visible effect
    // until on() is called (full brightness only).
    void setBrightness(uint8_t pct) {
        _brightness = _clamp(pct);
#ifndef CLED_NO_PWM
        if (_mode == Mode::IDLE && _currentPct > 0.0f)
            _apply((float)_brightness);
#endif
    }

    // count = -1 for infinite
    void blink(uint32_t onMs, uint32_t offMs, int32_t count = -1) {
        _blinkOnMs    = onMs;
        _blinkOffMs   = offMs;
        _blinkTotal   = count;
        _blinkLeft    = count;
        _blinkLit     = true;
        _blinkPhaseMs = onMs;
        _mode         = Mode::BLINKING;
        _apply((float)_brightness);
    }

    // In CLED_NO_PWM mode: fade commands snap to on/off immediately.
    void fadeOn(uint32_t durationMs) {
#ifdef CLED_NO_PWM
        on();
#else
        _startFade(_currentPct, 100.0f, durationMs);
#endif
    }

    void fadeOff(uint32_t durationMs) {
#ifdef CLED_NO_PWM
        off();
#else
        _startFade(_currentPct, 0.0f, durationMs);
#endif
    }

    void fadeTo(uint8_t targetPct, uint32_t durationMs) {
#ifdef CLED_NO_PWM
        // Treat as on if target >= 50%, off otherwise
        targetPct >= 50 ? on() : off();
#else
        _startFade(_currentPct, (float)_clamp(targetPct), durationMs);
#endif
    }

    void update(uint32_t elapsedMs) { _tick(elapsedMs); }

    // ── Queries ───────────────────────────────────────────────────────────────

    Mode    getMode()       const { return _mode; }
    uint8_t getBrightness() const { return _brightness; }
    float   getCurrentPct() const { return _currentPct; }
    bool    isIdle()        const { return _mode == Mode::IDLE; }
    bool    isOn()          const { return _mode == Mode::IDLE && _currentPct > 0.0f; }

private:
    uint             _gpio;
#ifndef CLED_NO_PWM
    PicoPwm          _pwm;
#endif
    uint8_t          _brightness;
    float            _currentPct;
    volatile Mode    _mode;

    // Blink
    uint32_t  _blinkOnMs, _blinkOffMs;
    int32_t   _blinkTotal;
    int32_t   _blinkLeft;
    uint32_t  _blinkPhaseMs;
    bool      _blinkLit;

    // Fade
    float     _fadeFrom, _fadeTo;
    uint32_t  _fadeDurationMs, _fadeElapsedMs;

    // ── Shared timer ──────────────────────────────────────────────────────────

    inline static repeating_timer_t  _timer;
    inline static CLedDriver*        _instances[MAX_LEDS] = {};
    inline static uint8_t            _count = 0;

    static bool _timerCB(repeating_timer_t*) {
        for (uint8_t i = 0; i < _count; i++)
            _instances[i]->_tick(TICK_MS);
        return true;
    }

    // ── Tick ──────────────────────────────────────────────────────────────────

    void _tick(uint32_t ms) {
        switch (_mode) {
            case Mode::BLINKING: _tickBlink(ms); break;
            case Mode::FADING:   _tickFade(ms);  break;
            default: break;
        }
    }

    void _tickBlink(uint32_t ms) {
        if (_blinkPhaseMs > ms) {
            _blinkPhaseMs -= ms;
            return;
        }
        _blinkLit = !_blinkLit;
        if (_blinkLit) {
            _blinkPhaseMs = _blinkOnMs;
            _apply((float)_brightness);
        } else {
            if (_blinkTotal > 0 && --_blinkLeft <= 0) {
                _mode = Mode::IDLE;
                _apply(0.0f);
                return;
            }
            _blinkPhaseMs = _blinkOffMs;
            _apply(0.0f);
        }
    }

    void _tickFade(uint32_t ms) {
        _fadeElapsedMs += ms;
        if (_fadeElapsedMs >= _fadeDurationMs) {
            _brightness = (uint8_t)(_fadeTo + 0.5f);
            _mode = Mode::IDLE;
            _apply(_fadeTo);
            return;
        }
        float t = (float)_fadeElapsedMs / (float)_fadeDurationMs;
        _apply(_fadeFrom + (_fadeTo - _fadeFrom) * t);
    }

    // ── Helpers ───────────────────────────────────────────────────────────────

    void _startFade(float from, float to, uint32_t durationMs) {
        _fadeFrom       = from;
        _fadeTo         = to;
        _fadeDurationMs = durationMs > 0 ? durationMs : 1;
        _fadeElapsedMs  = 0;
        _mode           = Mode::FADING;
    }

    void _apply(float pct) {
        _currentPct = pct < 0.0f ? 0.0f : (pct > 100.0f ? 100.0f : pct);
#ifdef CLED_NO_PWM
        gpio_put(_gpio, _currentPct >= 0.5f ? 1 : 0);
#else
        _pwm.setDutyPercentage((uint8_t)(_currentPct + 0.5f));
#endif
    }

    static uint8_t _clamp(uint8_t v) { return v > 100 ? 100 : v; }
};

// ─────────────────────────────────────────────────────────────────────────────
//  CLedParser — feed one character at a time
//
//  Format:  <leds><cmd>[<params>]  — groups separated by spaces
//
//  Leds:  '0' = all, '1'..'5' = individual (combinable: "135")
//
//  Commands / params:
//    O           on
//    C           off
//    Bn          blink n times (n=0 or B alone = infinite)
//    Bn:on       blink n times, on ms per phase (off = same)
//    Bn:on:off   blink n times, on ms on / off ms off
//    Sn          set brightness n% (0–100)
//    F+t         fade on  in t ms
//    F-t         fade off in t ms
//    Fn:t        fade to n% in t ms
//
//  Examples:
//    "12O"              leds 1,2 on
//    "345C"             leds 3,4,5 off
//    "0C"               all leds off
//    "15B5"             leds 1,5 blink 5 times (default timing)
//    "3B10:100:300"     led 3 blink 10×, 100ms on, 300ms off
//    "1S75"             led 1 brightness 75%
//    "2F+800"           led 2 fade on in 800ms
//    "2F-500"           led 2 fade off in 500ms
//    "4F30:600"         led 4 fade to 30% over 600ms
//    "12O 345C"         leds 1,2 on; leds 3,4,5 off
//    "0S50 0F+1000"     all leds to 50% brightness, then all fade on over 1s
// ─────────────────────────────────────────────────────────────────────────────
// !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
// !!!! NOTE! PWM doesn't work correctly on the current HIPI-board, so disbaled
// !!!! Fade will just turn on or off right now until new hardware is available.
// !!!! Remove the define above (CLED_NO_PWM) to restore full PWM support.
// !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!

class CLedParser {
public:
    static constexpr uint32_t DEFAULT_BLINK_ON  = 200;
    static constexpr uint32_t DEFAULT_BLINK_OFF = 200;
    static constexpr uint32_t DEFAULT_FADE_MS   = 500;

    CLedParser(CLedDriver* leds[], uint8_t count)
        : _leds(leds), _count(count) { _reset(); }

    // ── Feed one character ────────────────────────────────────────────────────

    void feed(char c) {
        if (c == '\r') return;

        // Space, newline, semicolon or null → end of group
        if (c == ' ' || c == '\n' || c == ';' || c == '\0') {
            _finalize();
            return;
        }

        // ── LED selection (only valid before a command is set) ────────────────
        if (_cmd == 0) {
            if (c == '0') {
                _ledMask = (1u << _count) - 1u;  // all LEDs
                return;
            }
            if (c >= '1' && c <= '5') {
                _ledMask |= 1u << (c - '1');
                return;
            }
        }

        // ── Command letter ────────────────────────────────────────────────────
        if (_cmd == 0 && (c=='O' || c=='C' || c=='B' || c=='S' || c=='F')) {
            _cmd = c;
            if (c == 'O' || c == 'C') _finalize();  // no params — apply now
            return;
        }

        // ── Fade direction (+/−) must immediately follow F ────────────────────
        if (_cmd == 'F' && _paramIdx == 0 && !_hasDigit) {
            if (c == '+' || c == '-') { _fadeDir = c; return; }
        }

        // ── Numeric digit ─────────────────────────────────────────────────────
        if (c >= '0' && c <= '9' && _cmd != 0) {
            _paramVal = _paramVal * 10 + (int32_t)(c - '0');
            _hasDigit = true;
            return;
        }

        // ── Colon: separator between parameters ───────────────────────────────
        if (c == ':' && _cmd != 0 && _paramIdx < 2) {
            _params[_paramIdx++] = _hasDigit ? _paramVal : 0;
            _paramVal = 0;
            _hasDigit = false;
            return;
        }

        // ── Anything else (e.g. a new LED digit after params with no space) ───
        if (_cmd != 0) {
            // A command is active -- finalize() resets _cmd, so re-processing
            // this same character will be interpreted in the "awaiting LED
            // selection" state.
            _finalize();
            feed(c);
        } else {
            // No active command and the character isn't a valid LED digit or
            // command letter -- ignore it silently instead of recursing
            // forever. This happens whenever non-LED-protocol data (e.g.
            // plain text) is sent to the LED device's HP-IL address.
        }
    }

    // Call after the last character of a string to flush any pending command
    void flush() { _finalize(); }

private:
    CLedDriver** _leds;
    uint8_t      _count;

    uint8_t  _ledMask;      // bit n = led[n] selected
    char     _cmd;          // active command letter, 0 = none
    int32_t  _params[3];    // up to 3 numeric parameters
    uint8_t  _paramIdx;     // next free param slot (0–2)
    int32_t  _paramVal;     // digits accumulating for current param
    bool     _hasDigit;     // true once at least one digit seen
    char     _fadeDir;      // '+', '-', or 0 (= fade-to)

    // ── Reset to idle ─────────────────────────────────────────────────────────

    void _reset() {
        _ledMask = 0;  _cmd = 0;  _fadeDir = 0;
        _params[0] = _params[1] = _params[2] = 0;
        _paramIdx = 0;  _paramVal = 0;  _hasDigit = false;
    }

    // ── Finalize and apply the current group ──────────────────────────────────

    void _finalize() {
        if (_cmd == 0) { _reset(); return; }

        // Flush the last param being accumulated
        if (_hasDigit)
            _params[_paramIdx] = _paramVal;

        for (uint8_t i = 0; i < _count; i++) {
            if (_ledMask & (1u << i))
                _apply(_leds[i]);
        }
        _reset();
    }

    void _apply(CLedDriver* led) {
        switch (_cmd) {

            case 'O':
                led->on();
                break;

            case 'C':
                led->off();
                break;

            case 'B': {
                // params: [0]=count, [1]=onMs, [2]=offMs
                int32_t  n   = _params[0];    // 0 → infinite
                uint32_t on  = _params[1] > 0 ? (uint32_t)_params[1] : DEFAULT_BLINK_ON;
                uint32_t off = _params[2] > 0 ? (uint32_t)_params[2] : on;  // off defaults to same as on
                led->blink(on, off, n > 0 ? n : -1);
                break;
            }

            case 'S':
                led->setBrightness((uint8_t)_clamp(_params[0], 0, 100));
                break;

            case 'F':
                if (_fadeDir == '+') {
                    led->fadeOn(_params[0] > 0 ? (uint32_t)_params[0] : DEFAULT_FADE_MS);
                } else if (_fadeDir == '-') {
                    led->fadeOff(_params[0] > 0 ? (uint32_t)_params[0] : DEFAULT_FADE_MS);
                } else {
                    // Fn:t  — fade to n% in t ms
                    uint8_t  pct = (uint8_t)_clamp(_params[0], 0, 100);
                    uint32_t ms  = _params[1] > 0 ? (uint32_t)_params[1] : DEFAULT_FADE_MS;
                    led->fadeTo(pct, ms);
                }
                break;
        }
    }

    static int32_t _clamp(int32_t v, int32_t lo, int32_t hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    }
};

extern CLedDriver ledPower;    // Power indicator
extern CLedDriver ledStatus;   // Status / activity
extern CLedDriver ledError;    // Error / alert
extern CLedDriver ledA;        // General purpose A
extern CLedDriver ledB;        // General purpose B
extern CLedDriver* leds[];

#endif//__LEDS_H__
