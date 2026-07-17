#pragma once
#include <cstdint>
#include <functional>

int  touchInit();
std::uint8_t touch_read();                                // existing, with printf debug
bool touch_get_point(std::uint16_t& x, std::uint16_t& y); // scaled position
bool touch_is_down();                                      // lightweight release check

// ── Debounced tap detection ─────────────────────────────────────────────────
//
// The GSL1680 only fires one IRQ edge per physical touch (the line stays
// high while held, so there's no second edge to wait for) -- so a single
// bad reading (capacitive noise/EMI) looks identical to a real tap at the
// point of the first sample. touch_poll() re-samples a short moment after
// the first detection and only fires the tap callback if the same
// approximate location is still reporting "down" -- filtering out
// single-sample glitches at the cost of a small (tens of ms) delay on
// real taps. Call touch_poll() once per main-loop iteration.

using TouchTapCallback     = std::function<void(std::uint16_t x, std::uint16_t y)>;
using TouchReleaseCallback = std::function<void()>;

// Invoked once, with the original (first-sample) coordinates, when a touch
// has passed the debounce confirmation.
void touch_set_tap_callback(TouchTapCallback cb);

// Invoked once when a confirmed touch's finger is detected as lifted.
// NOT called for a touch that never got confirmed (i.e. a filtered-out
// glitch) -- there's nothing to "release" in that case.
void touch_set_release_callback(TouchReleaseCallback cb);

// Call once per main-loop iteration. Consumes the IRQ flag, runs the
// confirm-resample debounce, and detects release via periodic polling
// (touch_is_down()) -- invoking whichever callbacks are registered.
void touch_poll();
