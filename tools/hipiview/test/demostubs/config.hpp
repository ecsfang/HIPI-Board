// Host stand-in for the settings (demo_view_test): the values the demo
// puts back, and nothing is saved
#pragma once
#include <cstdint>
namespace hipi {
class Config {
public:
    std::uint8_t fontSize() const { return 0; }
    std::uint16_t textColor() const { return 0xFFFF; }
    std::uint8_t columns() const { return 0; }
    bool v80Small() const { return false; }
    bool v80Forty() const { return false; }
    bool plotFit() const { return true; }
    bool plotPaperBlack() const { return false; }
    std::uint8_t clockStyle() const { return 0; }
};
}
