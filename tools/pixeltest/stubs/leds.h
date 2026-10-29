// Host stand-in for leds.h (pixeltest): only what pixels.cpp needs -- LED 5
// shares GPIO26 with the pixel strip (pixels_setFitted() turns it off)
#pragma once
#include <cstdint>
#define LED_PIN_5 26
struct CLedDriver {
    int offs = 0;
    void off() { ++offs; }
};
extern CLedDriver* leds[];
