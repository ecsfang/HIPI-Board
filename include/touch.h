#pragma once
#include <cstdint>

int  touchInit();
std::uint8_t touch_read();                              // befintlig, med printf-debug
bool touch_get_point(std::uint16_t& x, std::uint16_t& y); // skalad position
bool touch_is_down();                                     // latt release-koll
