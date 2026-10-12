// hwtest.h -- System -> Hardware test: a page to try out what's connected.
//
//   LEDS    "Fitted" (are they there?), "HP-IL on LED 3+4" (traffic shown
//           on LED 3/4, or the two left free for TFLEDS), a button per
//           LED 1-5, Chase (a light running 1 -> 5) and Off.
//   PIXELS  "Fitted", the number connected (1-16, = TFPIXEL's pixel
//           count), the pixels mirrored on the screen, Run (one pixel
//           running along, a new colour every round), Red / Green / Blue
//           / White (all of them, to check the strip's colour order) and
//           Off.
//   I2C     What answers on the bus, what it is and whether it works
//           (i2c_ident.h), looked at again every 3 s; Scan shows the
//           classic i2cdetect grid of all addresses.
//
// While the page is shown it owns the LEDs and the pixels (hwtest_io.h);
// leaving it switches them off and gives them back.
//
// Shown as the DisplayOutput::HwTest view (plotterview_showHwTest()).
#pragma once
#include "display_config.h"

namespace hipi {

void hwtest_init(DisplayDriver* display);
// The page opens / closes (plotterview.cpp): takes over / gives back the hardware
void hwtest_enter();
void hwtest_leave();
// The view (see plotterview_showHwTest())
void hwtest_drawAll();
// Main loop: the running lights, the I2C list
void hwtest_poll();
// A tap while the page is shown. Returns false if it hit nothing.
bool hwtest_tap(int x, int y);
// The i2cdetect grid (Scan, also from the view's menu)
void hwtest_showScan();

}  // namespace hipi
