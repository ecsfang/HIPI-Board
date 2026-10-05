// hpil_diag.h -- the HP-IL signals view (Display -> HP-IL signals), for
// finding out why the loop sometimes doesn't come up: like an oscilloscope,
// it shows the two HP-IL input lines (pIn = GPIO13, mIn = GPIO12, as named
// in the schematic), captured 8 million times a second when traffic starts
// (about 4 ms), with pulse widths and the frame HIPI's receiver decodes from
// them. A new capture every half second while the view is shown.
//
// Runs on PIO2 + one DMA channel, only READING the input pins -- the
// receiver on PIO0 isn't touched, and nothing runs while the view is closed.
#pragma once
#include "display_config.h"

namespace hipi {

void hpil_diag_init(DisplayDriver* display);
// Main loop: scope capture and drawing (only while the view is shown)
void hpil_diag_poll();
// The view (DisplayOutput::Signals, plotterview_showSignals())
void hpil_diag_drawAll();

}  // namespace hipi
