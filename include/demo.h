// demo.h -- System -> Demo: HIPI shows what it can do, on its own, over
// and over (7" panel only):
//
//   the display (HP 82163): a program listing, cursor addressing, inverse
//   video, the font sizes and colours -- then the same display as an
//   HP 92198 (80 and 40 columns, Roman-8); the cassette drive: the lid
//   opens, a cassette is picked, the reels turn, and DIR shows what's on
//   it; the plotter: the HIPI logo, a spirograph (with a screendump), a
//   function plot and an HP-IL loop drawing; the printer; a made-up HP-41
//   session in the analyzer, the loop map and the HP-IL signals; the LEDs
//   and pixels; a screen saver between the rounds.
//
// Nothing is saved or changed: settings are only changed on the screen
// and put back, the cassette isn't really chosen, the devices' loop
// addresses are put back. Afterwards the display, plot, paper and the
// analyzer are empty.
//
// Any touch (or hipiview button) stops it, and so does real HP-IL traffic
// (it's meant for showing, not for use alongside a controller); it goes
// back to the view it started from.
#pragma once
#include "display_config.h"
#include <functional>
#include <string>
#include <vector>

namespace hipi {

// The menu's cassette list, drawn by UiDialog (only drawn: nothing chosen)
struct DemoHooks {
    std::function<void(const std::vector<std::string>& files, int selected)> showPicker;
    std::function<void(int selected)> movePicker;
    std::function<void()> hidePicker;
};

void demo_init(DisplayDriver* display, const DemoHooks& hooks, const char* version);
void demo_start();
void demo_stop();
bool demo_running();
// Main loop
void demo_poll();
// A real HP-IL frame arrived (hipi.cpp): stops the demo (at the next poll)
void demo_hpilActivity();

}  // namespace hipi
