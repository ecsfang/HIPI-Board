#pragma once
// display_config.h -- the display controller this firmware drives, exposed
// under one common name (DisplayDriver) so the rest of the codebase
// (Screen.hpp, boardui.cpp, uidialog.hpp, etc.) doesn't spell out the chip.
// Include this instead of LT7683.hpp directly anywhere the *type* of the
// display driver is needed.
//
// HIPI has one panel: the 7" 1024x600 with an LT7683 display controller and
// an FT5316 capacitive touch controller (see touch.cpp).
#include "LT7683.hpp"

namespace hipi {
using DisplayDriver = LT7683;
}  // namespace hipi

#define DISPLAY_DEVICE "7\" with LT7683 controller"
