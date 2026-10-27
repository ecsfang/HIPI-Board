// hwtest.h -- System -> Hardware test: a page to try out what's connected.
//
//   LEDS    "Fitted" (are they there?), "HP-IL on LED 4" (traffic shown
//           on LED 4, or it's left free for TFLEDS), a button per
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
// Until a test of its own starts the page only looks: the LED buttons and
// the dots show what TFLEDS / TFPIXEL (and HP-IL activity) put on them
// (hwio::ledLevel(), hwio::pixelShown()) -- a program tried without LEDs
// or pixels fitted. An LED button or Chase takes the LEDs over, Run or a
// colour the pixels (hwio::takeLeds() / takePixels()); Off -- or leaving
// the page -- switches them off and gives them back.
//
// Shown as the DisplayOutput::HwTest view (views_showHwTest()).
#pragma once
#include "display_config.h"

namespace hipi {

void hwtest_init(DisplayDriver* display);
// The page opens / closes (views.cpp): takes over / gives back the hardware
void hwtest_enter();
void hwtest_leave();
// The view (see views_showHwTest())
void hwtest_drawAll();
// Main loop: the running lights, the I2C list
void hwtest_poll();
// A tap while the page is shown. Returns false if it hit nothing.
bool hwtest_tap(int x, int y);
// The i2cdetect grid (Scan, also from the view's menu)
void hwtest_showScan();
// The demo (demo.h): the LED chase and the pixel run started, as if their
// buttons were touched (leaving the page stops them)
void hwtest_demoRun();

}  // namespace hipi
