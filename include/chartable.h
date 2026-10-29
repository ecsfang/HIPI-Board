// chartable.h -- System -> Character table: the HP-41's character codes
// 0..127 (decimal), each with the character as the HP-41 display shows it
// (src/chartable_font.cpp, from resources/fonts/41charset_x.ttf). A page
// (views_isPage()): its X closes it.
//
// A touched character opens its details box: the character large, drawn
// from its lit segments (hp41_segments.h), its code in decimal, hex and
// binary, what the code is in ASCII, and what the HP 82162A printer prints
// for it (printer_font.h). < and > step to the code before / after
// (▲/▼ on the button strip too), CLOSE, its X, a touch beside it or X on
// the strip close it.
//
// Shown as the DisplayOutput::CharTable view (views_showCharTable()).
#pragma once
#include "display_config.h"

namespace hipi {

void chartable_init(DisplayDriver* display);
void chartable_drawAll();
// A touch on the page (not its X or menu button): a character -> its
// details box; in the box its buttons. False: nothing of the table there.
bool chartable_tap(int x, int y);
bool chartable_detailsOpen();
int chartable_selected();                 // the box's code, -1: none
void chartable_step(int by);              // the box: the code `by` on (wraps 127 -> 0)
void chartable_closeDetails();
void chartable_reset();                   // no box (the table opened afresh)

}  // namespace hipi
