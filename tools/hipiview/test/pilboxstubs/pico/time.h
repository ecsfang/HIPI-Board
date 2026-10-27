// Host stand-in for pico/time.h, for pilbox_test.cpp: a clock of its own
// that moves on 10 us with every tud_task() (and with sleeps), so waiting
// loops take simulated time, quickly and the same every run
#pragma once
#include <cstdint>
typedef std::uint64_t absolute_time_t;
inline std::uint64_t& pilClockUs() { static std::uint64_t t = 1000000; return t; }
inline absolute_time_t get_absolute_time() { return pilClockUs(); }
inline std::uint64_t time_us_64() { return pilClockUs(); }
inline std::uint32_t to_ms_since_boot(absolute_time_t t) { return static_cast<std::uint32_t>(t / 1000); }
inline absolute_time_t make_timeout_time_us(std::uint64_t us) { return get_absolute_time() + us; }
inline absolute_time_t make_timeout_time_ms(std::uint32_t ms) { return get_absolute_time() + ms * 1000ull; }
inline bool time_reached(absolute_time_t t) { return get_absolute_time() >= t; }
inline std::int64_t absolute_time_diff_us(absolute_time_t from, absolute_time_t to) {
    return static_cast<std::int64_t>(to) - static_cast<std::int64_t>(from);
}
inline void sleep_us(std::uint64_t us) { pilClockUs() += us; }
inline void sleep_ms(std::uint32_t ms) { pilClockUs() += ms * 1000ull; }
