#pragma once
#include "display_config.h"
#include <cstdint>
namespace hipi {
inline bool boardui_isMenuOpen() { return false; }
inline std::uint16_t boardui_buttonStripWidth() { return 50; }
inline int& pipBoxesClosedStub() { static int n = 0; return n; }
inline void boardui_closePipBoxes() { ++pipBoxesClosedStub(); }
inline int& splashStubShown() { static int n = 0; return n; }
inline void showSplashScreen(DisplayDriver*, const char*, std::uint32_t) { ++splashStubShown(); }
}
