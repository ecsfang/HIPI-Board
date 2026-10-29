// Host stand-in: a fake clock the test advances
#pragma once
#include "pico/types.h"
extern uint64_t g_fakeUs;
inline absolute_time_t get_absolute_time() { return g_fakeUs; }
inline uint32_t to_ms_since_boot(absolute_time_t t) { return static_cast<uint32_t>(t / 1000); }
inline uint64_t time_us_64() { return g_fakeUs; }
// A busy wait (pixels.cpp: the gap between two frames to the strip): the
// fake clock moves on, a microsecond a turn
inline void tight_loop_contents() { ++g_fakeUs; }
