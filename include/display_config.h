#pragma once
// display_config.h -- selects which display controller this build targets,
// and exposes it under one common name (DisplayDriver) so the rest of the
// codebase (Screen.hpp, boardui.cpp, uidialog.hpp, etc.) doesn't need to
// know or care which physical chip is actually on the board. Include this
// instead of RA8875.hpp/LT7683.hpp directly anywhere the *type* of the
// display driver is needed.
//
// Exactly one of DISPLAY_5INCH / DISPLAY_7INCH must end up defined.
//
//   DISPLAY_5INCH -- RA8875 display controller + GSL1680 capacitive touch
//   DISPLAY_7INCH -- LT7683 display controller + FT5316 capacitive touch
//
// Normally this is chosen on the command line via CMake, e.g.:
//   cmake -DHIPI_DISPLAY_PANEL=7INCH -B build
// (see CMakeLists.txt's HIPI_DISPLAY_PANEL cache variable) -- that adds a
// -DDISPLAY_5INCH or -DDISPLAY_7INCH compile definition before this header
// is ever processed, so the fallback default below is skipped entirely.
// The VS Code Pico extension doesn't pass that flag, so the line below is
// what it (and any other -D-less build) actually uses -- edit it directly
// if you're building that way and want the other panel.
//
// This only selects the *display* driver type. The touch controller
// selection (GSL1680 vs FT5316) is a separate #ifdef inside touch.cpp,
// gated on these same two macros -- see its own comment for why it isn't
// routed through a shared type alias the same way (touch.h's public API
// is free functions, not a class, so there's no equivalent "TouchDriver"
// type consumers need to spell out).
#if !defined(DISPLAY_5INCH) && !defined(DISPLAY_7INCH)
    #define DISPLAY_5INCH
    // #define DISPLAY_7INCH
#endif

#if defined(DISPLAY_5INCH) && defined(DISPLAY_7INCH)
    #error "display_config.h: define only one of DISPLAY_5INCH or DISPLAY_7INCH, not both"
#elif !defined(DISPLAY_5INCH) && !defined(DISPLAY_7INCH)
    #error "display_config.h: define one of DISPLAY_5INCH or DISPLAY_7INCH"
#endif

#if defined(DISPLAY_5INCH)
    #include "RA8875.hpp"
namespace hipi {
using DisplayDriver = RA8875;
}  // namespace hipi
#define DISPLAY_DEVICE "5\" with RA8875 controller"

#elif defined(DISPLAY_7INCH)
    #include "LT7683.hpp"
namespace hipi {
using DisplayDriver = LT7683;
}  // namespace hipi
#define DISPLAY_DEVICE "7\" with LT7683 controller"

#endif
