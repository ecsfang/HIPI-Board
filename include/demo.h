// demo.h -- System -> Demo: HIPI shows what it can do, on its own, over
// and over (7" panel only):
//
//   the display (HP 82163): a program listing, cursor addressing, inverse
//   video, the font sizes and colours -- then the same display as an
//   HP 92198 (80 and 40 columns, Roman-8); the cassette drive: the lid
//   opens, a cassette is picked, the reels turn, DIR shows what's on it
//   and the media map where it lies, CREATE MEDIA filled in (never
//   created), a made-up disk's media map BY TYPE / BY MACHINE; the plotter:
//   the HIPI logo, a spirograph (with a screendump), a function plot in the
//   Fit view and digitizing on it, an HP-GL error (the ERROR light, touched,
//   put out by OE), an HP-IL loop drawing; the printer (its
//   paper pulled up and torn off); a made-up HP-41 session on a made-up
//   loop (PILBOX's PC devices, devices after HIPI) in the analyzer,
//   the loop map (a card's details) and the HP-IL signals (the example,
//   without traffic); the clock if the RTC is there and set; the hardware
//   test page; the LEDs and pixels; a screen saver between the rounds.
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
    // CREATE MEDIA (File picker -> Create media...): opened, a control
    // touched (MediaCreateUi::Ctl, never Create), a key of MEDIA NAME's
    // keyboard, hidden -- nothing is written
    std::function<void()> showCreateMedia;
    std::function<void(int ctl)> createMediaControl;
    std::function<void(char key)> createMediaKey;
    std::function<void()> hideCreateMedia;
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
