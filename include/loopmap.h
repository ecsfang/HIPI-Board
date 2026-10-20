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
#include <cstdint>

namespace hipi {

// How long a card stays marked as talker / listener after the device was
// unaddressed (UNT, UNL, ...) -- an HP-41 often addresses a device for a
// few ms only. (The map is redrawn at most every 500 ms.)
constexpr std::uint32_t kLoopMapRoleHoldMs = 1200;

void loopmap_init(DisplayDriver* display);
// Main loop: takes in new frames, redraws when something changed (while shown)
void loopmap_poll();
// The view (see plotterview_showLoopMap())
void loopmap_drawAll();
// A tap while the map is shown: on a card -> its details box. In the box:
// GO TO <view> VIEW (a device with a view here) -> that view, where X on
// the button strip comes back to the map; X CLOSE or a tap beside the box
// -> closed. Returns false if the tap hit nothing of the map.
bool loopmap_tap(int x, int y);
// The middle of a device's card, as drawn last (the demo's "touch");
// false: no card for that address
bool loopmap_cardPoint(int addr, int& x, int& y);
// The details box from the button strip: OK = its view, X = close
bool loopmap_detailsOpen();
void loopmap_detailsOk();
void loopmap_closeDetails();
// Forgets the loop (the demo, after its made-up session); it is learnt
// again from the next auto-addressing
void loopmap_reset();

}  // namespace hipi
