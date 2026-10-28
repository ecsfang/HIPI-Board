// Host stand-in for the settings (clock_view_test)
#pragma once
#include <cstdint>
namespace hipi {
class Config {
public:
    std::uint8_t clockStyle() const { return style; }
    bool clock12h() const { return h12; }
    bool clockUs() const { return us; }
    std::uint8_t style = 0;
    bool h12 = false, us = false;
};
}
