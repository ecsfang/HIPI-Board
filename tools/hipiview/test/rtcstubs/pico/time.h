// pico/time.h for rtc_test: a clock the test moves by hand
#pragma once
#include <cstdint>
std::uint64_t& fakeTimeUs();
inline std::uint64_t time_us_64() { return fakeTimeUs(); }
