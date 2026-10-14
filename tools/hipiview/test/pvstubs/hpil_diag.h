// Host stand-in for the HP-IL signals view (no scope here): never any
// traffic, so it shows the example -- the real drawing (hpil_signals.cpp)
#pragma once
#include "hpil_signals.h"
namespace hipi {
inline DisplayDriver*& hpilDiagStubDisplay() { static DisplayDriver* d = nullptr; return d; }
inline void hpil_diag_init(DisplayDriver* d) { hpilDiagStubDisplay() = d; }
inline void hpil_diag_drawAll() { signals::draw(hpilDiagStubDisplay(), nullptr, 0, BitCells::Bands); }
}
