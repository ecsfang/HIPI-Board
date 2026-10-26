// hwtest_io.cpp -- see hwtest_io.h
#include "hwtest_io.h"
#include "leds.h"
#include "pixels.h"
#include "config.hpp"

extern CLedDriver* leds[];      // leds.cpp: LED 1..5
extern hipi::Config config;

namespace hipi {
namespace hwio {

void takeLeds() {
    hpil_leds_hold(true);
    for (int i = 0; i < 5; ++i) leds[i]->off();
}

void releaseLeds() {
    for (int i = 0; i < 5; ++i) leds[i]->off();
    hpil_leds_hold(false);
}

void takePixels() {
    pixelStrip.stopAllEffects();
    pixelStrip.clear();
    pixelStrip.show();
}

void releasePixels() {
    pixelStrip.clear();
    pixelStrip.show();
}

float ledLevel(int index) {
    if (index < 0 || index > 4) return 0.0f;
    return leds[index]->getCurrentPct();
}

void pixelShown(int index, std::uint8_t& r, std::uint8_t& g, std::uint8_t& b) {
    pixelStrip.bufferColor(static_cast<uint32_t>(index), r, g, b);
}

void setLed(int index, bool on) {
    if (index < 0 || index > 4) return;
    if (on) leds[index]->on();
    else    leds[index]->off();
}

void setPixel(int index, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    pixelStrip.setPixelRGB(static_cast<uint32_t>(index), r, g, b);
}

void showPixels() { pixelStrip.show(); }

int pixelCount() { return static_cast<int>(pixelStrip.count()); }
void setPixelCount(int n) { pixelStrip.setActiveCount(static_cast<uint32_t>(n)); }   // (saves it)

int pixelOrder() { return pixelStrip.colorOrder(); }
void setPixelOrder(int order) {
    if (order < 0 || order >= kPixelOrderCount) return;
    pixelStrip.setColorOrder(order);
    config.setPixelOrder(kPixelOrders[order]);
}

bool hpilLeds() { return hpil_leds_enabled(); }
void setHpilLeds(bool on) {
    hpil_leds_setEnabled(on);
    config.setHpilLeds(on);
}

bool ledsFitted() { return config.ledsFitted(); }
void setLedsFitted(bool f) { config.setLedsFitted(f); }
bool pixelsFitted() { return config.pixelsFitted(); }
void setPixelsFitted(bool f) {
    config.setPixelsFitted(f);
    pixels_setFitted(f);                             // GPIO26: strip or LED 5
}

int scanI2c(I2cFound* out, int max, bool found[128]) { return i2c_identify(out, max, found); }
std::string loadI2cChips() { return i2c_loadChips(); }

}  // namespace hwio
}  // namespace hipi
