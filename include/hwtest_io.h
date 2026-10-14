// hwtest_io.h -- the hardware the Hardware test page (hwtest.h) drives:
// LED 1-5, the pixel strip and the I2C bus. Kept apart from the page so
// the page can be tested on the PC (tools/hipiview, with a stand-in).
// The firmware's implementation is src/hwtest_io.cpp.
#pragma once
#include "i2c_ident.h"
#include <cstdint>
#include <string>

namespace hipi {
namespace hwio {

// The page opens: it takes over the LEDs (HP-IL activity held, TFLEDS'
// state dropped) and the pixels (TFPIXEL's effects stopped), all off
void begin();
// The page closes: LEDs and pixels off, HP-IL activity back (if chosen)
void end();

void setLed(int index, bool on);                 // index 0..4 = LED 1..5
void setPixel(int index, std::uint8_t r, std::uint8_t g, std::uint8_t b);
void showPixels();                               // sends what setPixel() set

// The number of pixels connected (TFPIXEL's pixel_count, saved)
int pixelCount();
void setPixelCount(int n);

// The pixels' colour byte order, 0..5 = RGB GRB BRG RBG GBR BGR
// (pixel_order, saved)
int pixelOrder();
void setPixelOrder(int order);

// HP-IL activity on LED 4 (hpil_leds, saved)
bool hpilLeds();
void setHpilLeds(bool on);

// Whether LEDs / pixels are fitted (saved; only the test page uses it)
bool ledsFitted();
void setLedsFitted(bool f);
bool pixelsFitted();
void setPixelsFitted(bool f);

// See i2c_identify()
int scanI2c(I2cFound* out, int max, bool found[128]);
// Reads the chip descriptions again (i2c_loadChips()): what was read
std::string loadI2cChips();

}  // namespace hwio
}  // namespace hipi
