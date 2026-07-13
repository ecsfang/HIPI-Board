#pragma once
#include <cstdint>

#define UI_DIALOG

int  touchInit();
std::uint8_t touch_read();                              // befintlig, med printf-debug
#ifdef UI_DIALOG
bool touch_get_point(std::uint16_t& x, std::uint16_t& y); // nytt: skalad position
bool touch_is_down();                                     // nytt: latt release-koll
#endif
