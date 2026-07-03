#ifndef __LEDS_H__
#define __LEDS_H__

#include <stdlib.h>
#include "pico/stdlib.h"
#include <stdio.h>
#include <pico/stdio.h>
#include <cstdint>
#include "hardware/pwm.h"
#include <string>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "pico/time.h"
#include "hardware/clocks.h"

#include <cstdio>

// ─── Pin definitions (edit here) ─────────────────────────────────────────────
#define LED_PIN_1 4
#define LED_PIN_2 5
#define LED_PIN_3 6
#define LED_PIN_4 22
#define LED_PIN_5 21

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

#endif//__LEDS_H__
