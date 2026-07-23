#pragma once
// plotterview.h -- switches the panel's full-screen output between the
// normal HP-41 text display (Screen) and a live rendering of the
// plotter's output (CPlotter). See boardui.h for the button
// strip/info-box/status-LED "chrome" this coordinates with.
//
// Usage (see pico_main.cpp):
//
//   plotter = new CPlotter("TFPLOT");   // (done inside hipi_init())
//   hp82163::plotterview_init(display, screen, plotter);
//
// The UiDialog "Display" menu calls plotterview_setOutput()/
// plotterview_clearPlotter()/plotterview_output() directly (see
// uidialog.hpp) -- there's no separate callback wiring needed here.

#include "RA8875.hpp"
#include "Screen.hpp"
#include "plotter.h"
#include <cstdint>

namespace hp82163 {

enum class DisplayOutput { Display, Plotter };

// Call once, after display/screen/plotter all exist (see hipi_init()).
void plotterview_init(RA8875* display, Screen* screen, CPlotter* plotter);

// Switches the panel's full-screen output. Handles suspending/resuming
// Screen and hiding/showing the button strip (Plotter mode uses the full
// 800x480 panel, so the strip is suppressed while it's active), and
// (re)drawing whichever output is now current.
void plotterview_setOutput(DisplayOutput mode);
DisplayOutput plotterview_output();

// Clears the plotter's drawing (both the retained segments() history and,
// if Plotter output is currently showing, the on-screen drawing too) --
// the "new paper" menu action. Safe to call regardless of which output
// mode is currently showing.
void plotterview_clearPlotter();

// Redraws the current plotter output from scratch (segments() replayed in
// full). Called internally when switching to Plotter output or when the
// menu closes back into it (to erase the menu box drawn on top), but
// exposed in case something else needs to force a refresh.
void plotterview_redraw();

// Redraws only a rectangular sub-region (screen pixel coordinates) of the
// current plotter output -- narrows the active window to that region
// first, so replaying every segment only actually draws the ones (or
// parts of ones) that fall inside it, then restores the full-screen
// active window Plotter mode normally runs with. Used by boardui.cpp's
// hideButtonStrip() to restore just the area the button bitmap occupied,
// instead of a full-screen redraw for what's normally a small strip.
void plotterview_redrawRegion(std::int16_t x0, std::int16_t y0,
                              std::int16_t w, std::int16_t h);

// True while Plotter output is the active full-screen view -- boardui's
// touch handling checks this to skip the normal button-strip wake/show
// logic (there's no strip to show) and instead treat any tap as opening
// the menu directly.
bool plotterview_isActive();

}  // namespace hp82163
