// backlight.h -- dims the display's backlight after a while without use,
// and brings it back as soon as something happens.
//
// "Use" is anything the user would see or do: new text from the HP-41
// (Screen::pr_char()), plotter drawing, the Tape view's lights and reels,
// new analyzer lines, and any touch. After kDimAfterMs without any of it,
// the backlight goes down to kDimLevel; the next activity restores the
// brightness set in the menu (Screen::brightness()).
// What happens is set in Settings -> Screen saver: dim, show the clock
// (clock.h; dims instead if there's no clock chip or it isn't set), or
// nothing.
#pragma once
#include <cstdint>

namespace hipi {

class Screen;

constexpr std::uint32_t kDimAfterMs = 10u * 60u * 1000u;   // 10 minutes
constexpr std::uint8_t  kDimLevel = 26;                     // ~10 % of 255
// The clock screen saver is dimmed too: faded down to kDimLevel over this
// time (0 = straight down)
constexpr std::uint32_t kClockFadeMs = 4000;

void backlight_init(Screen* screen);
// Something happened on the screen: restart the timer, undim if dimmed
void backlight_activity();
// A touch: like backlight_activity(), but returns true if the backlight was
// dimmed -- the touch only wakes the screen and should do nothing else
bool backlight_wakeByTouch();
// Main loop: dims once the time is up
void backlight_poll();
bool backlight_dimmed();          // the screen saver (dim or clock) is on
// Shows the clock right away, whatever the screen saver mode (Settings ->
// Screen saver -> Show clock now); ends like the screen saver
void backlight_showClockNow();

}  // namespace hipi
