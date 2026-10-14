// hpil_signals.h -- the HP-IL signals view's drawing (hpil_diag.h), apart
// from the scope hardware (PIO2 + DMA, hpil_diag.cpp), so it can be tried
// on the PC: decodes a capture like HIPI's receiver does and draws it --
// the first whole frame large, like the HP-IL specification's drawing,
// and the whole capture small.
//
// Without a real capture yet (no traffic on HP-IL IN since start-up) the
// view shows an example instead of an empty page: a made-up capture of an
// HP-41 printing "HIPI", clearly marked as an example. The first real
// capture replaces it.
#pragma once
#include "display_config.h"
#include <cstdint>

namespace hipi {

// How the bit cells of the decoded frame are marked (HP-IL signals ->
// Bit cells, config signal_cells=bands|lines|none): shaded bands behind
// every other cell, thin lines between the cells, or nothing at all.
enum class BitCells : std::uint8_t { Bands = 0, Lines = 1, None = 2 };

namespace signals {

constexpr std::uint32_t kSampleHz = 8000000;       // 0.125 us per sample
constexpr int kWords = 2048;                       // 16 samples each (2 bits a sample)
constexpr int kSamples = kWords * 16;              // ~4.1 ms
// Each sample: bit 0 = GPIO12, bit 1 = GPIO13. The receiver's "plus" and
// "minus" (IN_P_PIN / IN_M_PIN, hpil_pio.hpp -- checked there)
constexpr unsigned kScopeBase = 12;
constexpr unsigned kPinPlus = 12;
constexpr unsigned kPinMinus = 13;

// Draws the view. capture: kWords words of samples, or nullptr for the
// example (no traffic yet); captureNo: shown with a real capture.
// 7": drawn off-screen and copied in one go (no flicker).
void draw(DisplayDriver* d, const std::uint32_t* capture, std::uint32_t captureNo, BitCells cells);

// The example capture (kWords words): an HP-41 printing "HIPI" --
// LAD 01, DAB 48 49 50 49 0D 0A, UNL
void makeExample(std::uint32_t* buf);

}  // namespace signals
}  // namespace hipi
