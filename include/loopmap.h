// loopmap.h -- the HP-IL loop map: the loop drawn as a ring, with the
// controller and every device in loop order -- HIPI's own (yellow), the
// devices of a PC program behind PILBOX (blue) and other devices on the
// loop (orange). Each device's card shows its address, name, device ID and
// type; a tap on a card opens a box with everything known about it.
//
// HIPI only listens, so the map is built from the traffic it sees (the
// analyzer's capture, analyzer.h): the auto-addressing gives the addresses
// before HIPI, HIPI's own and the PC's; devices after HIPI appear when
// the controller addresses them; types and IDs when it asks them (SAI,
// SDI). The layout adapts to the number of devices (up to 30): big cards,
// compact cards, or small tiles.
//
// Shown via Display -> Loop map (DisplayOutput::LoopMap).
#pragma once
#include "display_config.h"

namespace hipi {

void loopmap_init(DisplayDriver* display);
// Main loop: takes in new frames, redraws when something changed (while shown)
void loopmap_poll();
// The view (see plotterview_showLoopMap())
void loopmap_drawAll();
// A tap while the map is shown: on a card -> its details, on the details
// box -> closes it. Returns false if the tap hit nothing of the map.
bool loopmap_tap(int x, int y);

}  // namespace hipi
