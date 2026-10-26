// hwio_stub.h -- the PC's stand-in for the Hardware test page's hardware
// (hwtest_io.h): remembers what the page did, and gives it a made-up I2C
// bus. Used by printer_view_test and hwtest_view_test.
#pragma once
#include "hwtest_io.h"
#include <cstdint>
#include <vector>

namespace hwstub {

struct Rgb { std::uint8_t r, g, b; };

// The page took them over (a test of its own) -- else it only looks;
// led[] and pixels[] are then what TFLEDS / TFPIXEL would have set
extern bool ledsTaken, pixelsTaken;
extern bool led[5];
extern std::vector<Rgb> pixels;     // as last shown
extern int pixelCount;
extern int pixelOrder;            // 0..5 = RGB GRB BRG RBG GBR BGR
extern int hpilLeds;
extern bool plotterLed;
extern bool ledsFitted, pixelsFitted;
extern int scans;                   // scanI2c() calls
// The I2C bus the page sees
extern std::vector<hipi::I2cFound> bus;

}  // namespace hwstub
