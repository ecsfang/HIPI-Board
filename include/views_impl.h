#pragma once
// views_impl.h -- what views.cpp and plotterview.cpp share. Not for the
// rest of HIPI: they use views.h.

#include "views.h"

namespace hipi::views_impl {

// views.cpp: the panel, Screen, and the view showing
extern DisplayDriver* display_;
extern Screen* screen_;
extern DisplayOutput output_;

// Switches the panel to `mode`, for device `dev` (views.cpp)
void switchView(DisplayOutput mode, CDevice* dev, bool splash = true);
// The yellow box: `text` at the built-in font's `scale`, for `ms` (views.cpp)
void showSplashBox(const char* text, std::uint8_t scale, std::uint32_t ms);

// plotterview.cpp: the Plotter view
void plotInit(CPlotter* plotter);          // (from views_init())
void plotRedraw();                         // the whole plot (views_redraw())
void plotRedrawRegion(std::int16_t x0, std::int16_t y0, std::int16_t w, std::int16_t h);
void plotPoll();                           // digitizing, the ERROR light (views_poll())

}  // namespace hipi::views_impl
