// Host stand-in: GPIO functions (pixels_setFitted(): the pin to the PIO or to LED 5's PWM)
#pragma once
#include "pico/types.h"
#include "hardware/pio.h"
enum gpio_function { GPIO_FUNC_PWM = 4, GPIO_FUNC_PIO0 = 6, GPIO_FUNC_PIO1 = 7 };
extern int g_gpioFunction[48];
inline void gpio_set_function(uint pin, gpio_function f) { g_gpioFunction[pin] = f; }
inline void pio_gpio_init(PIO pio, uint pin) { g_gpioFunction[pin] = pio == pio0 ? GPIO_FUNC_PIO0 : GPIO_FUNC_PIO1; }
