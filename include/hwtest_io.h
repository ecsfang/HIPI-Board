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

// The page only looks at the LEDs and pixels until one of its own tests
// starts: then it takes them over -- the LEDs (HP-IL activity held,
// TFLEDS' state dropped, all off) or the pixels (TFPIXEL's effects
// stopped, all off) -- and gives them back (off, HP-IL activity back)
// when the test ends or the page closes.
void takeLeds();
void releaseLeds();
void takePixels();
void releasePixels();
// Both at once (the demo's light show)
inline void begin() { takeLeds(); takePixels(); }
inline void end() { releaseLeds(); releasePixels(); }

// What's on them now, whoever drives them (TFLEDS, TFPIXEL, HP-IL
// activity, the page): an LED's level 0..100 % (blinking and fading as
// it goes), a pixel's colour as the strip is sent it (effects included)
float ledLevel(int index);                       // index 0..4 = LED 1..5
void pixelShown(int index, std::uint8_t& r, std::uint8_t& g, std::uint8_t& b);

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

// HP-IL activity on LED 4 (hpil_leds, saved): 0 off, 1 all frames,
// 2 messages (IDY frames ignored) -- leds.h kHpilLed...
int hpilLeds();
void setHpilLeds(int mode);
// The plotter's ERROR light on LED 3 (plotter_led, saved)
bool plotterLed();
void setPlotterLed(bool on);

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
