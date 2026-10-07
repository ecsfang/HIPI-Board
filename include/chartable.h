// chartable.h -- System -> Character table: the HP-41's character codes
// 0..127 (decimal), each with the character as the HP-41 display shows it
// (from resources/fonts/41charset_x.ttf, re-arranged by HP-41 code by
// scripts/hp41_charset.py into src/chartable_font.cpp). A help page only:
// shown over the whole screen, any touch (or X) closes it.
//
// Shown as the DisplayOutput::CharTable view (plotterview_showCharTable()).
#pragma once
#include "display_config.h"

namespace hipi {

void chartable_init(DisplayDriver* display);
void chartable_drawAll();

}  // namespace hipi
