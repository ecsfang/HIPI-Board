// saver_anim.h -- the animated screen savers (besides the clock):
//
//  Rain   HP-IL mnemonics (RFC, TAD 01, SDA, DAB 41...) falling down the
//         screen in columns, Matrix style -- taken from the real traffic on
//         the loop (analyzer.h) when there is any, made up otherwise.
//  Goose  The HP-41's "running" goose: flocks of 1..5, in different sizes
//         and speeds, flying across the screen from left to right.
//
// Shown in the screen saver view (see clock.h / plotterview_showClock()),
// in the clock's style (dark, or LCD).
#pragma once
#include "display_config.h"

namespace hipi {

enum class AnimKind { Rain, Goose };

// lcd: LCD style (dark on light), else dark style (light on black)
void anim_start(AnimKind kind, DisplayDriver* display, bool lcd);

// The animations "Random" picks from -- add new ones here
constexpr AnimKind kRandomAnims[] = { AnimKind::Rain, AnimKind::Goose };
void anim_drawAll();        // full redraw
void anim_step();           // main loop: the next bit of animation (a few ms at most)

}  // namespace hipi
