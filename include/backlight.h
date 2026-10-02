// backlight.h -- dims the display's backlight after a while without use,
// and brings it back as soon as something happens.
//
// "Use" is anything the user would see or do: new text from the HP-41
// (Screen::pr_char()), plotter drawing, the Tape view's lights and reels,
// new analyzer lines, and any touch. After kDimAfterMs without any of it,
// the backlight goes down to kDimLevel; the next activity restores the
// brightness set in the menu (Screen::brightness()).
#pragma once
#include <cstdint>

namespace hipi {

class Screen;

constexpr std::uint32_t kDimAfterMs = 10u * 60u * 1000u;   // 10 minutes
constexpr std::uint8_t  kDimLevel = 10; //26                    // ~10 % of 255

void backlight_init(Screen* screen);
// Something happened on the screen: restart the timer, undim if dimmed
void backlight_activity();
// A touch: like backlight_activity(), but returns true if the backlight was
// dimmed -- the touch only wakes the screen and should do nothing else
bool backlight_wakeByTouch();
// Main loop: dims once the time is up
void backlight_poll();
bool backlight_dimmed();

}  // namespace hipi
