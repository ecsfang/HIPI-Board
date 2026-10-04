// clock.h -- the clock screen saver: time in big digits over the whole
// screen (HH:MM, 24 h or 12 h + AM/PM), the date below (EU yyyy-mm-dd or
// US mm/dd/yyyy) and the weekday and month in English. White on black, or
// dark on a light background like an old LCD. Updated every minute, from
// the DS3231 real-time clock (rtc.h); the font is pre-rendered from the
// "HP41 Character Set Xtended" TrueType font (glyph_font.h).
//
// Shown by the backlight timer (backlight.h) when Settings -> Screen saver
// is set to Clock; any activity returns to the previous view.
//
// The time is set over the USB debug console: type
//     time 2026-10-03 14:05        (seconds optional: 14:05:30)
// and Enter; "time" alone shows the current time.
#pragma once

// No DS3231 yet: 1 = a stand-in clock that starts at the firmware's build
// time and counts with the Pico's timer ("time ..." still sets it, until
// the next restart). Set to 0 once the RTC module is connected.
#ifndef HIPI_FAKE_RTC
#define HIPI_FAKE_RTC 1
#endif
#include "display_config.h"
#include <string>

namespace hipi {

void clock_init(DisplayDriver* display);
// What the screen saver view shows: the clock, or an animation
// (saver_anim.h). Set before plotterview_showClock().
enum class SaverShow { Clock, Rain, Goose };
// lcd: style for the animations (the clock always uses Settings' style)
void clock_setShow(SaverShow show, bool lcd);
// RTC found and set: the clock can be shown
bool clock_available();
// The clock view (see plotterview_showClock())
void clock_drawAll();
// Main loop: redraws when the minute changes (while shown), and handles
// the "time" command on the USB console
void clock_poll();
// Current time as text (for the console / log), "" without RTC
std::string clock_nowText();

}  // namespace hipi
