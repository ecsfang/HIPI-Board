// Host stand-in for the Pico SDK's clock: the test moves it (fakeNowUs)
#pragma once
#include <cstdint>
typedef std::uint64_t absolute_time_t;
extern std::uint64_t fakeNowUs;
inline absolute_time_t get_absolute_time() { return fakeNowUs; }
inline absolute_time_t make_timeout_time_ms(std::uint32_t ms) { return fakeNowUs + ms * 1000ull; }
inline bool time_reached(absolute_time_t t) { return fakeNowUs >= t; }
inline void sleep_ms(std::uint32_t ms) { fakeNowUs += ms * 1000ull; }
