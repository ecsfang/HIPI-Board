#pragma once
#include "display_config.h"
#include <cstdint>
namespace hipi {
inline bool boardui_isMenuOpen() { return false; }
inline int& splashStubShown() { static int n = 0; return n; }
inline void showSplashScreen(DisplayDriver*, const char*, std::uint32_t) { ++splashStubShown(); }
}
